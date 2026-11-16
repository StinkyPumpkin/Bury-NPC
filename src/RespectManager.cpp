#include "pch.h"
#include "RespectManager.h"
#include "Settings.h"
#include "PromptManager.h"
#include "PickupManager.h"

#include <mutex>
#include <thread>
#include <chrono>
#include <functional>
#include <cctype>

namespace
{
	RE::TESObjectCELL*          g_cleanupCell = nullptr;
	RE::BGSSoundDescriptorForm* g_soundForm = nullptr;
	RE::TESBoundObject*         g_graveActivator = nullptr;
	bool                        g_hasUIExtensions = false;

	// Vanilla shovels (Skyrim.esm) — gate the Bury prompt/action.
	RE::TESBoundObject*         g_shovel1 = nullptr;  // Shovel01 0xF5D05
	RE::TESBoundObject*         g_shovel2 = nullptr;  // Shovel02 0xF5D06

	// Our menu-less isSmelter "dig" furniture (BuryTakeBodies.esp 0x835).
	RE::TESFurniture*           g_animFurniture = nullptr;

	// "Remains" container (BuryTakeBodies.esp 0x836): a buried unique NPC's
	// inventory waits in one of these in the holding cell until it is dug up.
	RE::TESObjectCONT*          g_remains = nullptr;
	// "Burial Urn" container (0x837, the vanilla Nordic canopic jar model):
	// used instead when the NPC was buried as an ash pile.
	RE::TESObjectCONT*          g_remainsUrn = nullptr;
	// A dug-up urn stands at half size (refScale is in percent).
	constexpr std::uint16_t     kUrnScalePct = 50;
	// The same "Remains" container for a woman (0x83B): its base object is how
	// Dig Up knows to raise the female draugr, with no extra save data.
	RE::TESObjectCONT*          g_remainsF = nullptr;
	// Dead draugr a buried corpse is dug up as, on the plain draugr body (no
	// armor): 0x839 PFR_RemainsDraugr (male), 0x83A PFR_RemainsDraugrF (female).
	RE::TESNPC*                 g_remainsDraugr = nullptr;
	RE::TESNPC*                 g_remainsDraugrF = nullptr;
	// Only people (ActorTypeNPC, Skyrim.esm 0x13794) keep their belongings in the grave.
	RE::BGSKeyword*             g_actorTypeNPC = nullptr;

	// Bumped on every new game / load. Work that waits (the dig animation, the
	// bury-box check) holds FormIDs from the game it started in; a temporary ID
	// can belong to a different object after a load, so it stops if this moved.
	std::atomic<std::uint32_t>  g_generation{ 0 };

	// ------------------------------------------------------------------
	// Bury-in-progress state.  Bury pops a UIExtensions text-entry box; the
	// typed message arrives via a SKSE mod event and the grave is placed when
	// that box closes, so we stash what we need between those callbacks.
	// ------------------------------------------------------------------
	std::mutex   g_buryMutex;
	bool         g_burying = false;
	RE::FormID   g_buryBodyID = 0;
	std::string  g_buryDeadName;
	std::string  g_buryMessage;
	bool         g_buryConfirmed = false;
	bool         g_buryBoxOpened = false;   // the text box actually appeared
	// Edit Epitaph uses the same box: the grave being re-engraved (0 = a bury)
	// and its current epitaph, put into the box when it opens.
	RE::FormID   g_buryEditGraveID = 0;
	std::string  g_buryPrefill;

	// ------------------------------------------------------------------
	// Move a reference to another cell (used to send the corpse to the
	// vanilla WIDeadBodyCleanupCell so it disappears).
	// ------------------------------------------------------------------
	void MoveRefTo(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_targetCell, RE::TESWorldSpace* a_worldspace,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_rotation)
	{
		using func_t = void (*)(RE::TESObjectREFR*, const RE::ObjectRefHandle&,
			RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
		static REL::Relocation<func_t> func{ RELOCATION_ID(56227, 56626) };
		func(a_ref, RE::ObjectRefHandle{}, a_targetCell, a_worldspace, a_position, a_rotation);
	}

	void MoveRefTo(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_targetCell,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_rotation)
	{
		MoveRefTo(a_ref, a_targetCell, nullptr, a_position, a_rotation);
	}

	// Stand a freshly placed grave upright on the ground. PlaceObjectAtMe copied the
	// corpse's rotation, and a ragdoll's X/Y rotation can be anything, so on rough
	// ground the headstone came out tilted or upside down. Keep only a heading
	// (turned toward the player) and, outdoors, sit it on the terrain under the body.
	void StandGraveUpright(RE::TESObjectREFR* a_grave, RE::TESObjectREFR* a_body)
	{
		if (!a_grave || !a_body) return;

		RE::NiPoint3 pos = a_body->GetPosition();
		float heading = a_body->GetAngleZ();
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const RE::NiPoint3 toPlayer = player->GetPosition() - pos;
			if (toPlayer.x != 0.0f || toPlayer.y != 0.0f) {
				heading = std::atan2(toPlayer.x, toPlayer.y);
			}
		}

		// Terrain height only in exteriors; skip it when the body lies well above or
		// below the terrain (a rock, a bridge, a cave mouth) so the grave isn't sunk.
		if (auto* cell = a_body->GetParentCell(); cell && !cell->IsInteriorCell()) {
			float land = 0.0f;
			if (auto* tes = RE::TES::GetSingleton(); tes && tes->GetLandHeight(pos, land) &&
				std::abs(land - pos.z) < 80.0f) {
				pos.z = land;
			}
		}

		MoveRefTo(a_grave, a_grave->GetParentCell(), a_grave->GetWorldspace(),
			pos, RE::NiPoint3{ 0.0f, 0.0f, heading });
	}

	void PlayTurnUndeadSound()
	{
		if (!g_soundForm) return;
		auto* descriptor = g_soundForm->soundDescriptor;
		if (!descriptor) return;
		auto* audioManager = RE::BSAudioManager::GetSingleton();
		if (!audioManager) return;

		RE::BSSoundHandle soundHandle;
		audioManager->BuildSoundDataFromDescriptor(soundHandle, descriptor, 0x1A);
		if (soundHandle.IsValid()) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && player->Get3D()) {
				soundHandle.SetObjectToFollow(player->Get3D());
			}
			soundHandle.Play();
		}
	}

	// Ash piles (turned-undead remains): a NON-actor ref whose base name
	// contains "ash". --Claude 2026-09-18: the actor exclusion is the fix for
	// living NPCs with "ash" in their name (Rashaal Taussa) getting the corpse
	// prompts and being removed by Lay to Rest.
	// The engine links an ash pile (an activator) to the actor it came from with
	// ExtraAshPileRef - the same link QuickLoot follows to loot one. The old test
	// matched "ash" in the base NAME, so an Ash Yam, Ashen Grass Pod or bottle of
	// Ashfire Mead on the ground got the corpse prompts.
	RE::Actor* AshPileActor(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || a_ref->As<RE::Actor>()) return nullptr;
		auto* base = a_ref->GetBaseObject();
		if (!base || !base->Is(RE::FormType::Activator)) return nullptr;
		auto owner = a_ref->extraList.GetAshPileRef().get();
		return owner ? owner->As<RE::Actor>() : nullptr;
	}

	bool IsAshPile(RE::TESObjectREFR* a_ref)
	{
		return AshPileActor(a_ref) != nullptr;
	}

	// The actor behind a corpse or an ash pile (holds the inventory and the name).
	RE::Actor* CorpseActor(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) return nullptr;
		if (auto* actor = a_ref->As<RE::Actor>()) return actor;
		return AshPileActor(a_ref);
	}

	// A dead draugr this mod raised from a grave ("Remains of <Name>").
	bool IsRemainsDraugr(RE::TESObjectREFR* a_ref)
	{
		auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
		auto* base = actor ? actor->GetActorBase() : nullptr;
		return base && ((g_remainsDraugr && base == g_remainsDraugr) || (g_remainsDraugrF && base == g_remainsDraugrF));
	}

	// A burial urn this mod dug up (or one still waiting in a grave).
	bool IsUrn(RE::TESObjectREFR* a_ref)
	{
		return a_ref && g_remainsUrn && a_ref->GetBaseObject() == g_remainsUrn;
	}

	// The dead person's own display name. A raised draugr is called "Remains of
	// <Name>"; that prefix is dropped, so burying it again engraves the name and
	// digging it up again does not stack "Remains of Remains of".
	std::string DeadNameOf(RE::TESObjectREFR* a_ref)
	{
		std::string name;
		if (const char* n = a_ref ? a_ref->GetDisplayFullName() : nullptr) name = n;
		constexpr std::string_view prefix = "Remains of ";
		constexpr std::string_view ashes = "Ashes of ";
		if (IsRemainsDraugr(a_ref) && name.starts_with(prefix)) name.erase(0, prefix.size());
		else if (IsUrn(a_ref) && name.starts_with(ashes)) name.erase(0, ashes.size());
		return name;
	}

	// Half size. Set while the urn is not loaded (in the holding cell, or right
	// before it is moved out), so its 3D and collision load at this size; the
	// change flag keeps the scale in the save.
	void MakeUrnSmall(RE::TESObjectREFR* a_urn)
	{
		if (!a_urn) return;
		auto& rd = a_urn->GetReferenceRuntimeData();
		if (rd.refScale == kUrnScalePct) return;
		rd.refScale = kUrnScalePct;
		a_urn->AddChange(RE::TESObjectREFR::ChangeFlags::kScale);
	}

	struct ItemMove
	{
		RE::TESBoundObject* obj;
		RE::ExtraDataList*  xList;
		std::int32_t        count;
	};

	// Something a player can see and take in a loot menu. Mods put hidden
	// non-playable armor on NPCs (TNG's genital cover, SMP physics objects), and a
	// freshly placed NPC can still list an unresolved leveled list from its base
	// container: none of those show in a menu, so in a container they could never
	// be emptied (the urn would never turn into its jar).
	bool IsLootable(RE::TESBoundObject* a_obj)
	{
		if (!a_obj || a_obj->Is(RE::FormType::LeveledItem)) return false;
		if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
			return (armo->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0;
		}
		if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) {
			return !weap->weaponData.flags.any(RE::TESObjectWEAP::Data::Flag::kNonPlayable);
		}
		if (auto* ammo = a_obj->As<RE::TESAmmo>()) {
			return !ammo->data.flags.any(RE::AMMO_DATA::Flag::kNonPlayable);
		}
		return true;
	}

	std::vector<ItemMove> CollectItems(RE::TESObjectREFR* a_from, bool a_questOnly)
	{
		std::vector<ItemMove> moves;
		if (!a_from) return moves;
		auto inv = a_from->GetInventory();
		for (auto& [obj, data] : inv) {
			auto& [count, entry] = data;
			if (!obj || count <= 0 || !entry) continue;
			if (!IsLootable(obj)) continue;   // hidden: stays with the body
			const bool quest = entry->IsQuestObject();
			if (a_questOnly && !quest) continue;
			// Other mods tag bodies with worthless marker items (RSE's "Bandit On
			// Player Token", ShoulderCarryState's "dead-still"): a MiscObject worth
			// 0 gold that weighs nothing. Leave those behind with the body.
			// Checked by form type: keys are TESObjectMISC too in CommonLib (TESKey
			// derives from it) and most keys are worth 0 and weigh 0.
			if (!quest && obj->Is(RE::FormType::Misc)) {
				auto* misc = static_cast<RE::TESObjectMISC*>(obj);
				if (misc->value == 0 && misc->weight == 0.0f) continue;
			}
			std::int32_t listed = 0;
			if (entry->extraLists) {
				for (auto* xList : *entry->extraLists) {
					if (!xList || listed >= count) continue;
					const std::int32_t n = (std::max)(1, xList->GetCount());
					moves.push_back({ obj, xList, n });
					listed += n;
				}
			}
			if (count > listed) moves.push_back({ obj, nullptr, count - listed });
		}
		return moves;
	}

	std::int32_t CountOf(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_obj)
	{
		auto counts = a_ref->GetInventoryCounts([a_obj](RE::TESBoundObject& a_o) { return &a_o == a_obj; });
		const auto it = counts.find(a_obj);
		return it != counts.end() ? it->second : 0;
	}

	bool HasQuestItems(RE::Actor* a_actor)
	{
		return a_actor && !CollectItems(a_actor, true).empty();   // read only
	}

	// Two ash effects on one death leave two piles linked to the same actor; bury
	// one and the other could be buried again (a second grave, an empty urn).
	// Find the pile's twins in its cell so they leave with it. The cell is walked
	// under its own lock (TES::ForEachReference walks without it and has crashed).
	std::vector<RE::NiPointer<RE::TESObjectREFR>> FindTwinAshPiles(RE::TESObjectREFR* a_pile, RE::Actor* a_actor)
	{
		std::vector<RE::NiPointer<RE::TESObjectREFR>> twins;
		auto* cell = a_pile ? a_pile->GetParentCell() : nullptr;
		if (!cell || !a_actor || !cell->IsAttached()) return twins;
		auto& rd = cell->GetRuntimeData();
		RE::BSSpinLockGuard lock(rd.spinLock);
		for (const auto& refPtr : rd.references) {
			auto* ref = refPtr.get();
			if (ref && ref != a_pile && AshPileActor(ref) == a_actor) twins.push_back(refPtr);
		}
		return twins;
	}

	void SendTwinsAway(const std::vector<RE::NiPointer<RE::TESObjectREFR>>& a_twins)
	{
		for (const auto& twin : a_twins) {
			if (!twin) continue;
			if (g_cleanupCell) {
				MoveRefTo(twin.get(), g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, twin->GetAngle());
			}
			twin->Disable();
			logger::info("Ash pile twin {:08X} sent away with its first pile", twin->GetFormID());
		}
	}

	// Send the actor behind an ash pile away as well; it stays in the world,
	// unseen, after the pile itself is moved.
	void SendAshActorAway(RE::Actor* a_actor)
	{
		if (!a_actor) return;
		if (g_cleanupCell) {
			MoveRefTo(a_actor, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, a_actor->GetAngle());
		}
		a_actor->Disable();
	}

	// Lay to Rest and Bury send the corpse away; anything a quest needs comes to
	// the player so the quest can't get stuck. Like the Remains, this waits until
	// the body has left the loaded area (taking items off a corpse that is still
	// drawn, then sending it away in the same frame, hung the game). Each stack
	// keeps its own extra-data list, which carries the quest link, and the
	// inventory is read again before every move.
	// Main thread, body already out of the loaded area. Returns the stacks moved.
	std::int32_t MoveQuestItemsToPlayer(RE::Actor* a_actor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!a_actor || !player) return 0;
		std::vector<RE::TESBoundObject*> stuck;
		std::int32_t moved = 0;
		for (int guard = 0; guard < 256; ++guard) {
			const auto moves = CollectItems(a_actor, true);
			const auto it = std::find_if(moves.begin(), moves.end(), [&](const ItemMove& a_m) {
				return std::find(stuck.begin(), stuck.end(), a_m.obj) == stuck.end();
			});
			if (it == moves.end()) break;
			const std::int32_t before = CountOf(a_actor, it->obj);
			a_actor->RemoveItem(it->obj, it->count, RE::ITEM_REMOVE_REASON::kStoreInContainer, it->xList, player);
			if (CountOf(a_actor, it->obj) < before) {
				++moved;
			} else {
				stuck.push_back(it->obj);   // could not be moved; don't try it again
			}
		}
		logger::info("Quest items: moved {} stacks from {:08X} to the player ({} refused)", moved, a_actor->GetFormID(), stuck.size());
		if (moved > 0) {
			RE::DebugNotification("You took the quest items they carried.");
		}
		return moved;
	}

	// Main thread: every item that can be looted, per item type with no extra-data
	// list (the game picks the stacks, the way Papyrus RemoveItem does).
	std::size_t MoveAllItems(RE::Actor* a_actor, RE::TESObjectREFR* a_to)
	{
		std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> stacks;
		for (const auto& m : CollectItems(a_actor, false)) {
			auto it = std::find_if(stacks.begin(), stacks.end(),
				[&](const auto& s) { return s.first == m.obj; });
			if (it == stacks.end()) {
				stacks.emplace_back(m.obj, m.count);
			} else {
				it->second += m.count;
			}
		}
		for (const auto& [obj, count] : stacks) {
			a_actor->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, a_to);
		}
		return stacks.size();
	}

	void GiveQuestItemsLater(RE::FormID a_actorID)
	{
		const std::uint32_t gen = g_generation.load();
		std::thread([a_actorID, gen]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			auto* task = SKSE::GetTaskInterface();
			if (!task) return;
			task->AddTask([a_actorID, gen]() {
				if (g_generation.load() != gen) return;   // a save was loaded meanwhile
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actorID);
				if (!actor) {
					logger::warn("Quest items: body {:08X} is gone - nothing handed over", a_actorID);
					return;
				}
				MoveQuestItemsToPlayer(actor);
			});
		}).detach();
	}

	bool IsValidCorpse(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) return false;
		auto* actor = a_ref->As<RE::Actor>();
		if (actor) return actor->IsDead();   // a living actor is never a corpse
		return IsAshPile(a_ref);
	}

	// ------------------------------------------------------------------
	// Place the inscribed gravestone at the body and remove the body.  Runs on
	// the main thread via the task queue.
	// ------------------------------------------------------------------
	// A buried unique NPC keeps their belongings: everything they carried goes into
	// a "Remains" container parked in our holding cell, linked to the grave, and
	// Dig Up brings it back. The NPC itself is removed as before.
	// Someone buried as an ash pile comes back up as a burial urn of their ashes.
	RE::TESObjectCONT* RemainsBase(bool a_ashes, bool a_female)
	{
		if (a_ashes && g_remainsUrn) return g_remainsUrn;
		return (a_female && g_remainsF) ? g_remainsF : g_remains;
	}

	bool IsFemale(RE::Actor* a_actor)
	{
		auto* npc = a_actor ? a_actor->GetActorBase() : nullptr;
		return npc && npc->GetSex() == RE::SEX::kFemale;
	}

	// A unique PERSON who still carries something: their grave will hold Remains.
	// Creatures (a unique horse, spider or dragon with its death loot) do not:
	// their loot leaves with the body like any other corpse's. A raised draugr
	// ("Remains of <Name>") is a person here too, so it can be buried again.
	bool WillKeepRemains(RE::Actor* a_actor, bool a_ashes)
	{
		auto* npc = a_actor ? a_actor->GetActorBase() : nullptr;
		const bool person = IsRemainsDraugr(a_actor) || (g_actorTypeNPC && a_actor->HasKeyword(g_actorTypeNPC));
		return npc && npc->IsUnique() && person && RemainsBase(a_ashes, false) && PickupManager::GetHoldingMarker() &&
		       !CollectItems(a_actor, false).empty();
	}

	// Returns the container's FormID, or 0 when the body is not a unique NPC.
	RE::FormID CreateRemains(RE::TESObjectREFR* a_grave, RE::Actor* a_actor, bool a_ashes)
	{
		auto* marker = PickupManager::GetHoldingMarker();
		auto* base = RemainsBase(a_ashes, IsFemale(a_actor));
		if (!a_grave || !WillKeepRemains(a_actor, a_ashes)) return 0;

		RE::NiPointer<RE::TESObjectREFR> remains = marker->PlaceObjectAtMe(base, true);
		if (!remains) return 0;
		const std::string name = DeadNameOf(a_actor);
		const std::string label = (base == g_remainsUrn) ? "Ashes of " : "Remains of ";
		remains->SetDisplayName(RE::BSFixedString(label + (name.empty() ? "the fallen" : name)), true);
		if (base == g_remainsUrn) MakeUrnSmall(remains.get());
		PickupManager::RegisterBuriedBody(a_grave->GetFormID(), remains->GetFormID());
		return remains->GetFormID();
	}

	// Fill the Remains once the body has left the loaded area. 1.6.0 took the worn
	// gear off the corpse while it was still drawn and sent the body away in the
	// same frame; every unique bury done that way hung the game moments later
	// inside the memory allocator (a non-unique bury, which moves no items, was
	// fine). Now the body goes first, and half a second later its items move per
	// item type with no extra-data list, so the game picks the stacks itself the
	// way Papyrus RemoveItem does.
	void MoveRemainsLater(RE::FormID a_actorID, RE::FormID a_remainsID)
	{
		const std::uint32_t gen = g_generation.load();
		std::thread([a_actorID, a_remainsID, gen]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			auto* task = SKSE::GetTaskInterface();
			if (!task) return;
			task->AddTask([a_actorID, a_remainsID, gen]() {
				if (g_generation.load() != gen) return;   // a save was loaded meanwhile
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actorID);
				auto* remains = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_remainsID);
				if (!actor || !remains) {
					logger::warn("Remains: body {:08X} or container {:08X} is gone - nothing moved", a_actorID, a_remainsID);
					return;
				}
				const std::size_t types = MoveAllItems(actor, remains);
				logger::info("Remains: moved {} item types from {:08X} into {:08X}", types, a_actorID, a_remainsID);
			});
		}).detach();
	}

	void PlaceGraveAndBury(RE::FormID a_bodyID, std::string a_engraving)
	{
		auto* body = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_bodyID);
		if (!body) return;

		// A dug-up urn goes back into a new grave whole, with what is left in it:
		// it waits in the holding cell as that grave's remains until dug up again.
		if (IsUrn(body)) {
			auto* marker = PickupManager::GetHoldingMarker();
			RE::NiPointer<RE::TESObjectREFR> grave =
				(g_graveActivator && marker) ? body->PlaceObjectAtMe(g_graveActivator, true) : nullptr;
			if (!grave) {
				logger::warn("Bury: no grave for urn {:08X}", a_bodyID);
				body->SetActivationBlocked(false);
				return;
			}
			StandGraveUpright(grave.get(), body);
			grave->SetDisplayName(RE::BSFixedString(a_engraving), true);
			PlayTurnUndeadSound();
			body->SetActivationBlocked(false);
			body->MoveTo(marker);
			PickupManager::RegisterBuriedBody(grave->GetFormID(), a_bodyID);
			logger::info("Bury: grave {:08X} for urn {:08X}", grave->GetFormID(), a_bodyID);
			return;
		}

		auto* actor = CorpseActor(body);

		RE::FormID remainsID = 0;
		if (g_graveActivator) {
			// Graves are permanent: they stay where they were dug (a grave holding
			// Remains is also the only link to them). Each costs a small entry in the save.
			const bool ashes = actor && actor != body;
			RE::NiPointer<RE::TESObjectREFR> grave =
				body->PlaceObjectAtMe(g_graveActivator, true);
			if (grave) {
				StandGraveUpright(grave.get(), body);
				grave->SetDisplayName(RE::BSFixedString(a_engraving), true);
				remainsID = CreateRemains(grave.get(), actor, ashes);
				logger::info("Bury: grave {:08X} for {:08X}, remains {:08X}", grave->GetFormID(), a_bodyID, remainsID);
			} else {
				logger::warn("Bury: PlaceObjectAtMe returned null for {:08X}", a_bodyID);
			}
		}
		const bool questItems = !remainsID && HasQuestItems(actor);
		const auto twins = (actor && actor != body) ? FindTwinAshPiles(body, actor)
		                                            : std::vector<RE::NiPointer<RE::TESObjectREFR>>{};

		PlayTurnUndeadSound();
		body->SetActivationBlocked(false);
		if (g_cleanupCell) {
			MoveRefTo(body, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, body->GetAngle());
		}
		body->Disable();
		SendTwinsAway(twins);

		// Items leave only a body that is no longer loaded (see MoveRemainsLater).
		if (actor && (remainsID || questItems)) {
			if (actor != body) SendAshActorAway(actor);
			if (remainsID) {
				MoveRemainsLater(actor->GetFormID(), remainsID);
			} else {
				GiveQuestItemsLater(actor->GetFormID());
			}
		}

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("Bury: buried {:08X} — '{}'", a_bodyID, a_engraving);
		}
	}

	constexpr std::string_view kHereLies = "Here lies ";
	constexpr std::string_view kEpitaphSep = " — ";

	// No epitaph: "Here lies <Name>". With one: "<Name> — <epitaph>".
	std::string BuildEngraving(const std::string& a_deadName, const std::string& a_message)
	{
		const std::string hereLies = std::string(kHereLies) + (a_deadName.empty() ? std::string("the fallen") : a_deadName);
		// v1.4.3: the NAME is engraved automatically — if the player typed the name again
		// (field report: grave read "Ivana — Ivana"), treat it as no message.
		auto iequals = [](const std::string& a, const std::string& b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
					return false;
			return true;
		};
		if (a_message.empty() || iequals(a_message, a_deadName) || iequals(a_message, hereLies)) {
			return hereLies;
		}
		std::string engraving = a_deadName.empty() ? hereLies : a_deadName;
		engraving += kEpitaphSep;
		engraving += a_message;
		return engraving;
	}

	// The reverse of BuildEngraving, for Edit Epitaph: the name and the epitaph
	// on a grave (older graves read just "<Name>").
	void SplitEngraving(const std::string& a_engraving, std::string& a_deadName, std::string& a_message)
	{
		std::string rest = a_engraving;
		if (rest.starts_with(kHereLies)) rest.erase(0, kHereLies.size());
		const auto sep = rest.find(kEpitaphSep);
		if (sep == std::string::npos) {
			a_deadName = rest;
			a_message.clear();
		} else {
			a_deadName = rest.substr(0, sep);
			a_message = rest.substr(sep + kEpitaphSep.size());
		}
		if (a_deadName == "the fallen") a_deadName.clear();
	}

	// ------------------------------------------------------------------
	// Play the smelter-shovel "dig" animation, then place the grave.  The player
	// uses a menu-less isSmelter furniture (PFR_BuryAnimMarker) spawned at their
	// feet; after a delay the grave appears, the body is removed, the player
	// stands, and the marker is cleaned up.  Falls back to an instant bury when
	// the animation is off or the furniture is missing.
	// ------------------------------------------------------------------
	// Play the dig animation, then run a_finish on the main thread. Without the
	// animation (setting off, furniture missing) a_finish runs at once.
	void PlayDigThen(std::function<void()> a_finish)
	{
		auto& settings = PFR::Settings::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		RE::NiPointer<RE::TESObjectREFR> furn;
		if (settings.buryAnimation.load() && g_animFurniture && player) {
			furn = player->PlaceObjectAtMe(g_animFurniture, false);
		}
		if (!furn) {
			a_finish();
			return;
		}
		const RE::FormID furnID = furn->GetFormID();
		furn->ActivateRef(player, 0, nullptr, 1, false);  // player starts shovelling

		int ms = static_cast<int>(settings.buryAnimationDelay.load() * 1000.0f);
		if (ms < 0) ms = 0;
		const std::uint32_t gen = g_generation.load();

		// Wait out the animation off the main thread, then finish on it.
		std::thread([a_finish = std::move(a_finish), furnID, ms, gen]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			auto* task = SKSE::GetTaskInterface();
			if (!task) return;
			task->AddTask([a_finish, furnID, gen]() {
				if (g_generation.load() != gen) return;   // a save was loaded meanwhile
				a_finish();
				if (auto* furnRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(furnID)) {
					if (auto* p = RE::PlayerCharacter::GetSingleton()) {
						furnRef->ActivateRef(p, 0, nullptr, 1, false);  // stand up / exit
					}
					furnRef->Disable();
					furnRef->SetDelete(true);
				}
			});
		}).detach();
	}

	void DoBuryWithAnimation(RE::FormID a_bodyID, std::string a_engraving)
	{
		PlayDigThen([a_bodyID, a_engraving]() { PlaceGraveAndBury(a_bodyID, a_engraving); });
	}

	// ------------------------------------------------------------------
	// Finalize a bury once the text box has closed: build the inscription from
	// the typed message and place the grave (or, on cancel, just un-block the
	// body).  Clears the bury state.
	// ------------------------------------------------------------------
	void FinalizeBury()
	{
		RE::FormID  bodyID, editGraveID;
		std::string deadName, message;
		bool        confirmed;
		{
			std::scoped_lock lk(g_buryMutex);
			if (!g_burying) return;
			bodyID = g_buryBodyID;
			editGraveID = g_buryEditGraveID;
			deadName = g_buryDeadName;
			message = g_buryMessage;
			confirmed = g_buryConfirmed;
			g_burying = false;
			g_buryBodyID = 0;
			g_buryEditGraveID = 0;
			g_buryDeadName.clear();
			g_buryMessage.clear();
			g_buryPrefill.clear();
			g_buryConfirmed = false;
			g_buryBoxOpened = false;
		}

		if (editGraveID) {
			// Edit Epitaph: Esc keeps the old inscription.
			if (!confirmed) return;
			std::string engraving = BuildEngraving(deadName, message);
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([editGraveID, engraving]() {
					auto* grave = RE::TESForm::LookupByID<RE::TESObjectREFR>(editGraveID);
					if (!grave || grave->GetBaseObject() != g_graveActivator) return;
					grave->SetDisplayName(RE::BSFixedString(engraving), true);
					logger::info("Edit Epitaph: grave {:08X} now reads '{}'", editGraveID, engraving);
					// The crosshair keeps the old name until it loses the grave. Forget the
					// current target: the next frame picks the grave again as a new target
					// and reads the new name. (VR keeps its pick data elsewhere: skipped.)
					if (!REL::Module::IsVR()) {
						if (auto* pick = RE::CrosshairPickData::GetSingleton();
							pick && pick->target.get().get() == grave) {
							pick->target = RE::ObjectRefHandle{};
						}
					}
				});
			}
			return;
		}

		if (!confirmed) {
			// Cancelled — leave the body where it lies.
			if (auto* body = RE::TESForm::LookupByID<RE::TESObjectREFR>(bodyID)) {
				body->SetActivationBlocked(false);
			}
			return;
		}

		std::string engraving = BuildEngraving(deadName, message);
		if (auto* task = SKSE::GetTaskInterface()) {
			task->AddTask([bodyID, engraving]() { DoBuryWithAnimation(bodyID, engraving); });
		}
	}

	// ------------------------------------------------------------------
	// Open the UIExtensions text-entry box (registered custom menu name
	// "textentrymenu") via the Papyrus VM.
	// ------------------------------------------------------------------
	void OpenBuryTextEntry(bool a_edit)
	{
		// v1.4.3: tell the player what the box is for — the NAME goes on
		// automatically, the box is only the epitaph.
		RE::DebugNotification(a_edit ? "Edit the epitaph — leave it empty for \"Here lies\" and the name."
		                             : "Write your epitaph — the name is engraved automatically.");

		// If the box never appears (UIExtensions installed but its menu failed),
		// the body would stay blocked from activation for good, and that block is
		// saved. After a few seconds without the box, bury with the name only.
		const std::uint32_t gen = g_generation.load();
		std::thread([gen]() {
			std::this_thread::sleep_for(std::chrono::seconds(4));
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([gen]() {
					if (g_generation.load() != gen) return;
					RE::FormID bodyID = 0, editGraveID = 0;
					std::string deadName;
					{
						std::scoped_lock lk(g_buryMutex);
						if (!g_burying || g_buryBoxOpened) return;
						bodyID = g_buryBodyID;
						editGraveID = g_buryEditGraveID;
						deadName = g_buryDeadName;
						g_burying = false;
						g_buryBodyID = 0;
						g_buryEditGraveID = 0;
						g_buryDeadName.clear();
						g_buryMessage.clear();
						g_buryPrefill.clear();
						g_buryConfirmed = false;
					}
					if (editGraveID) {
						logger::warn("Edit Epitaph: the text box did not open for grave {:08X}", editGraveID);
						RE::DebugNotification("The epitaph box didn't open.");
						return;
					}
					logger::warn("Bury: the epitaph box did not open; burying {:08X} with the name only", bodyID);
					RE::DebugNotification("The epitaph box didn't open - buried with the name only.");
					if (auto* body = RE::TESForm::LookupByID<RE::TESObjectREFR>(bodyID)) {
						body->SetActivationBlocked(false);
					}
					DoBuryWithAnimation(bodyID, BuildEngraving(deadName, ""));
				});
			}
		}).detach();

		SKSE::GetTaskInterface()->AddUITask([]() {
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) return;
			auto args = RE::MakeFunctionArguments(
				RE::BSFixedString("textentrymenu"), static_cast<std::int32_t>(0));
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
			vm->DispatchStaticCall("UI", "OpenCustomMenu", args, cb);
		});
	}

	// ------------------------------------------------------------------
	// A one-line tip above the text box ("Here lies <Name> — write an
	// epitaph:"). The UIExtensions box has no label of its own, so we add a
	// TextField to its _root.textEntry clip once the SWF reports it loaded.
	// Scaleform work, so this runs as a UI task.
	// ------------------------------------------------------------------
	void AddEpitaphTip(const std::string& a_deadName, bool a_edit, const std::string& a_prefill)
	{
		// Edit Epitaph: start from the epitaph already on the grave.
		if (!a_prefill.empty()) {
			if (auto* ui = RE::UI::GetSingleton()) {
				if (auto menu = ui->GetMenu("CustomMenu"); menu && menu->uiMovie) {
					const RE::GFxValue arg[1]{ a_prefill.c_str() };
					menu->uiMovie->Invoke("_root.textEntry.setTextEntryMenuText", nullptr, arg, 1);
				}
			}
		}

		auto* ui = RE::UI::GetSingleton();
		auto menu = ui ? ui->GetMenu("CustomMenu") : nullptr;
		auto* movie = menu ? menu->uiMovie.get() : nullptr;
		if (!movie) return;

		RE::GFxValue entry;
		if (!movie->GetVariable(&entry, "_root.textEntry") || !entry.IsDisplayObject()) return;

		RE::GFxValue bounds;
		const RE::GFxValue boundsArg[1]{ entry };
		if (!entry.Invoke("getBounds", &bounds, boundsArg, 1) || !bounds.IsObject()) return;
		RE::GFxValue xMin, xMax, yMin;
		bounds.GetMember("xMin", &xMin);
		bounds.GetMember("xMax", &xMax);
		bounds.GetMember("yMin", &yMin);
		if (!xMin.IsNumber() || !xMax.IsNumber() || !yMin.IsNumber()) return;

		const double width = xMax.GetNumber() - xMin.GetNumber();
		const RE::GFxValue tfArgs[6]{ "pfrEpitaphTip", 1000.0, xMin.GetNumber(),
			yMin.GetNumber() - 40.0, width, 34.0 };
		RE::GFxValue tip;
		entry.Invoke("createTextField", &tip, tfArgs, 6);
		if (!tip.IsDisplayObject() && !tip.IsObject()) {
			entry.GetMember("pfrEpitaphTip", &tip);
		}
		if (!tip.IsObject() && !tip.IsDisplayObject()) return;

		RE::GFxValue fmt;
		movie->CreateObject(&fmt, "TextFormat");
		fmt.SetMember("font", "$EverywhereMediumFont");
		fmt.SetMember("size", 22.0);
		fmt.SetMember("color", 16777215.0);
		fmt.SetMember("align", "center");
		const RE::GFxValue fmtArg[1]{ fmt };
		tip.SetMember("embedFonts", true);
		tip.SetMember("selectable", false);
		tip.Invoke("setNewTextFormat", nullptr, fmtArg, 1);

		const std::string text = "Here lies " + (a_deadName.empty() ? std::string("the fallen") : a_deadName) +
			(a_edit ? " — edit the epitaph:" : " — write an epitaph:");
		tip.SetMember("text", text.c_str());
	}

	// ------------------------------------------------------------------
	// Listens for the text box's result (SKSE mod event) and its close
	// (menu event) to finalize the bury.
	// ------------------------------------------------------------------
	class BuryDialogSink :
		public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
		public RE::BSTEventSink<SKSE::ModCallbackEvent>
	{
	public:
		static BuryDialogSink& GetSingleton()
		{
			static BuryDialogSink instance;
			return instance;
		}

		void Register()
		{
			if (auto* ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
			}
			if (auto* src = SKSE::GetModCallbackEventSource()) {
				src->AddEventSink(this);
			}
		}

		// UIExtensions fires "UITextEntryMenu_TextChanged" (strArg = final text)
		// when the user CONFIRMS.  On cancel it does not fire.
		RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event,
			RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
		{
			if (!a_event) return RE::BSEventNotifyControl::kContinue;
			if (a_event->eventName == "UITextEntryMenu_LoadMenu") {
				std::string deadName, prefill;
				bool active, edit;
				{
					std::scoped_lock lk(g_buryMutex);
					active = g_burying;
					edit = g_buryEditGraveID != 0;
					deadName = g_buryDeadName;
					prefill = g_buryPrefill;
				}
				if (active) {
					SKSE::GetTaskInterface()->AddUITask([deadName, edit, prefill]() { AddEpitaphTip(deadName, edit, prefill); });
				}
			} else if (a_event->eventName == "UITextEntryMenu_TextChanged") {
				// The box's Cancel sends this too, with EMPTY text (its Accept never sends
				// empty text): that is a cancel, the body stays. A blank epitaph arrives as
				// whitespace (see ConfirmEmptyEpitaphIfBlank) and is trimmed away.
				std::string msg = a_event->strArg.c_str();
				std::scoped_lock lk(g_buryMutex);
				if (g_burying && !msg.empty()) {
					const auto first = msg.find_first_not_of(" \t\r\n");
					const auto last = msg.find_last_not_of(" \t\r\n");
					g_buryMessage = first == std::string::npos ? std::string() : msg.substr(first, last - first + 1);
					g_buryConfirmed = true;
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		// The text box opens as "CustomMenu"; when it closes we finalize.
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (!a_event) return RE::BSEventNotifyControl::kContinue;
			if (a_event->opening && a_event->menuName == "CustomMenu") {
				std::scoped_lock lk(g_buryMutex);
				if (g_burying) g_buryBoxOpened = true;
			}
			if (!a_event->opening && a_event->menuName == "CustomMenu") {
				bool active;
				{
					std::scoped_lock lk(g_buryMutex);
					active = g_burying;
				}
				if (active) FinalizeBury();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ------------------------------------------------------------------
	// Enter on an EMPTY box. The text box ignores Enter while it holds no text
	// (it stays open, nothing is sent), so "no epitaph" could not be confirmed.
	// While our box is up, Enter is checked here: if the field is empty the box
	// is closed for it and the bury / edit goes ahead with no epitaph
	// ("Here lies <Name>"). With text in the field the box handles Enter itself.
	// ------------------------------------------------------------------
	void ConfirmEmptyEpitaphIfBlank()
	{
		{
			std::scoped_lock lk(g_buryMutex);
			if (!g_burying || !g_buryBoxOpened || g_buryConfirmed) return;
		}
		auto* ui = RE::UI::GetSingleton();
		auto menu = ui ? ui->GetMenu("CustomMenu") : nullptr;
		auto* movie = menu ? menu->uiMovie.get() : nullptr;
		if (!movie) return;
		RE::GFxValue text;
		if (!movie->GetVariable(&text, "_root.textEntry.TextInputInstance.textField.text") || !text.IsString()) return;
		const std::string_view s = text.GetString();
		if (s.find_first_not_of(" \t\r\n") != std::string_view::npos) return;   // has text: the box sends it
		// Let the box accept it itself: give it a single space (its Accept refuses an
		// empty field) and call its own onAccept. That sends the text (read as "no
		// epitaph"), turns text input OFF again and closes the box. Closing the menu
		// from outside skipped skse.AllowTextInput(false), which left the game in
		// text-input mode: the player could look around but not move.
		const RE::GFxValue space[1]{ " " };
		const bool set = movie->Invoke("_root.textEntry.setTextEntryMenuText", nullptr, space, 1);
		if (set && movie->Invoke("_root.textEntry.onAccept", nullptr, nullptr, 0)) return;
		// A text box without those functions: do what its Accept does, by hand.
		{
			std::scoped_lock lk(g_buryMutex);
			if (!g_burying || g_buryConfirmed) return;
			g_buryMessage.clear();
			g_buryConfirmed = true;
		}
		const RE::GFxValue off[1]{ false };
		movie->Invoke("_global.skse.AllowTextInput", nullptr, off, 1);
		const RE::GFxValue name[1]{ "CustomMenu" };
		movie->Invoke("_global.skse.CloseMenu", nullptr, name, 1);
	}

	class EpitaphEnterSink : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static EpitaphEnterSink& GetSingleton()
		{
			static EpitaphEnterSink instance;
			return instance;
		}

		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
			RE::BSTEventSource<RE::InputEvent*>*) override
		{
			if (!a_events) return RE::BSEventNotifyControl::kContinue;
			for (auto* e = *a_events; e; e = e->next) {
				auto* btn = e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton ? e->AsButtonEvent() : nullptr;
				if (!btn || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !btn->IsDown()) continue;
				const auto key = btn->GetIDCode();
				if (key != 0x1C && key != 0x9C) continue;   // Enter, numpad Enter
				bool open;
				{
					std::scoped_lock lk(g_buryMutex);
					open = g_burying && g_buryBoxOpened && !g_buryConfirmed;
				}
				if (open) {
					SKSE::GetTaskInterface()->AddUITask([]() { ConfirmEmptyEpitaphIfBlank(); });
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ------------------------------------------------------------------
	// A dug-up urn behaves like the raised draugr: it stays a container (aim at
	// it for QuickLoot, open it, empty or not) and it has physics - it falls,
	// rolls and can be grabbed. A container loads held in place, so every time
	// an urn's 3D loads (dig up, drop after Pick Up, cell or save load) it is
	// switched to dynamic motion. Urns waiting in the holding cell never load.
	// ------------------------------------------------------------------
	class UrnPhysicsSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
	{
	public:
		static UrnPhysicsSink& GetSingleton()
		{
			static UrnPhysicsSink instance;
			return instance;
		}

		void Register()
		{
			if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<RE::TESObjectLoadedEvent>(this);
			}
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
			RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
		{
			if (!a_event || !a_event->loaded || !g_remainsUrn) return RE::BSEventNotifyControl::kContinue;
			const RE::FormID id = a_event->formID;
			if (!IsUrn(RE::TESForm::LookupByID<RE::TESObjectREFR>(id))) return RE::BSEventNotifyControl::kContinue;
			// Next frame: the collision object is in the world by then.
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([id]() {
					auto* urn = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
					if (!IsUrn(urn) || urn->IsDisabled() || !urn->Is3DLoaded()) return;
					urn->SetMotionType(RE::TESObjectREFR::MotionType::kDynamic, true);
				});
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// Where dug-up remains come to rest: on the ground in front of the grave,
	// toward the player (on the grave's own spot they would sit inside the
	// headstone). a_heading gets the direction facing the player.
	RE::NiPoint3 SpotBeforeGrave(RE::TESObjectREFR* a_grave, float& a_heading)
	{
		RE::NiPoint3 pos = a_grave->GetPosition();
		a_heading = a_grave->GetAngleZ();
		if (auto* p = RE::PlayerCharacter::GetSingleton()) {
			RE::NiPoint3 d = p->GetPosition() - pos;
			d.z = 0.0f;
			const float len = d.Length();
			if (len > 1.0f) {
				pos += d * ((std::min)(70.0f, len * 0.5f) / len);
				a_heading = std::atan2(d.x, d.y);
			}
		}
		if (auto* cell = a_grave->GetParentCell(); cell && !cell->IsInteriorCell()) {
			float land = 0.0f;
			if (auto* tes = RE::TES::GetSingleton(); tes && tes->GetLandHeight(pos, land) &&
				std::abs(land - pos.z) < 80.0f) {
				pos.z = land;
			}
		}
		return pos;
	}

	// A buried corpse comes back up as a dead draugr named "Remains of <Name>"
	// carrying what was buried with them: a real body with ragdoll physics, so
	// Take Body, Bury and Lay to Rest all work on it. It is permanent and is
	// never cleaned up. It is killed in the same task it is placed in, before it
	// is drawn or its AI gets a turn, with no killer (no crime). The belongings
	// move from the hidden Remains container (nothing worn) into a fresh corpse.
	bool RaiseRemainsDraugr(RE::TESObjectREFR* a_remains, RE::TESObjectREFR* a_grave)
	{
		if (!a_remains || !a_grave) return false;
		const bool female = g_remainsF && a_remains->GetBaseObject() == g_remainsF;
		RE::TESNPC* base = (female && g_remainsDraugrF) ? g_remainsDraugrF : g_remainsDraugr;
		if (!base) return false;
		RE::NiPointer<RE::TESObjectREFR> ref = a_grave->PlaceObjectAtMe(base, true);
		auto* draugr = ref ? ref->As<RE::Actor>() : nullptr;
		if (!draugr) return false;

		float heading = 0.0f;
		RE::NiPoint3 pos = SpotBeforeGrave(a_grave, heading);
		pos.z += 20.0f;   // drop onto the ground rather than start inside it
		MoveRefTo(draugr, a_grave->GetParentCell(), a_grave->GetWorldspace(), pos, RE::NiPoint3{ 0.0f, 0.0f, heading });
		draugr->KillImpl(nullptr, 0.0f, true, true);

		const char* name = a_remains->GetDisplayFullName();
		draugr->SetDisplayName(RE::BSFixedString(name && *name ? name : "Remains"), true);

		std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> stacks;
		for (const auto& [obj, count] : a_remains->GetInventoryCounts()) {
			if (obj && count > 0) stacks.emplace_back(obj, count);
		}
		for (const auto& [obj, count] : stacks) {
			a_remains->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, draugr);
		}
		a_remains->Disable();
		a_remains->SetDelete(true);
		logger::info("Dig Up: remains {:08X} raised as dead {} draugr {:08X} ({} item types)",
			a_remains->GetFormID(), female ? "female" : "male", draugr->GetFormID(), stacks.size());
		return true;
	}

}

namespace RespectManager
{
	void OnDataLoaded()
	{
		g_cleanupCell = RE::TESForm::LookupByEditorID<RE::TESObjectCELL>("WIDeadBodyCleanupCell");
		g_soundForm = RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(0x00057C65);

		// Grave activator ships in BuryTakeBodies.esp (PFR_GraveActivator, 0x834)
		// — no Pay Your Respects dependency.
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (dh) {
			g_graveActivator = dh->LookupForm<RE::TESObjectACTI>(0x000834, "BuryTakeBodies.esp");
			g_hasUIExtensions = dh->LookupModByName("UIExtensions.esp") != nullptr;
			// Vanilla shovels — MiscObjects in Skyrim.esm (named "Shovel").
			g_shovel1 = dh->LookupForm<RE::TESObjectMISC>(0x00F5D05, "Skyrim.esm");
			g_shovel2 = dh->LookupForm<RE::TESObjectMISC>(0x00F5D06, "Skyrim.esm");
			// Our dig-animation furniture.
			g_animFurniture = dh->LookupForm<RE::TESFurniture>(0x000835, "BuryTakeBodies.esp");
			// Container for a buried unique NPC's belongings.
			g_remains = dh->LookupForm<RE::TESObjectCONT>(0x000836, "BuryTakeBodies.esp");
			g_remainsUrn = dh->LookupForm<RE::TESObjectCONT>(0x000837, "BuryTakeBodies.esp");
			g_remainsDraugr = dh->LookupForm<RE::TESNPC>(0x000839, "BuryTakeBodies.esp");
			g_remainsDraugrF = dh->LookupForm<RE::TESNPC>(0x00083A, "BuryTakeBodies.esp");
			g_remainsF = dh->LookupForm<RE::TESObjectCONT>(0x00083B, "BuryTakeBodies.esp");
		}
		g_actorTypeNPC = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013794);

		BuryDialogSink::GetSingleton().Register();
		UrnPhysicsSink::GetSingleton().Register();
		if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
			idm->AddEventSink(&EpitaphEnterSink::GetSingleton());
		}

		logger::info("RespectManager: cleanupCell={} sound={} graveActivator={} remains={}/{} urn={} draugr={}/{} ActorTypeNPC={} UIExtensions={}",
			g_cleanupCell != nullptr, g_soundForm != nullptr, g_graveActivator != nullptr,
			g_remains != nullptr, g_remainsF != nullptr, g_remainsUrn != nullptr,
			g_remainsDraugr != nullptr, g_remainsDraugrF != nullptr, g_actorTypeNPC != nullptr, g_hasUIExtensions);

		if (!g_graveActivator) {
			logger::warn("RespectManager: BuryTakeBodies.esp grave activator not found — "
				"enable BuryTakeBodies.esp for the Bury option");
		}
	}

	bool PlayerHasShovel()
	{
		if (!g_shovel1 && !g_shovel2) return false;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return false;
		auto counts = player->GetInventoryCounts();
		if (g_shovel1) {
			auto it = counts.find(g_shovel1);
			if (it != counts.end() && it->second > 0) return true;
		}
		if (g_shovel2) {
			auto it = counts.find(g_shovel2);
			if (it != counts.end() && it->second > 0) return true;
		}
		return false;
	}

	bool CanResurrect(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) return false;
		auto* actor = a_ref->As<RE::Actor>();
		return actor && actor->IsDead();  // ash piles / non-actors excluded
	}

	// A raised draugr would come back as a hostile draugr named after the dead,
	// so its prompt offers Take Body only.
	bool IsRaisedRemains(RE::TESObjectREFR* a_ref)
	{
		return IsRemainsDraugr(a_ref);
	}

	bool IsUrnRef(RE::TESObjectREFR* a_ref)
	{
		return IsUrn(a_ref);
	}

	bool PickUpAshPile(RE::FormID a_refID)
	{
		auto* pile = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
		if (!IsAshPile(pile)) return false;
		if (!PFR::Settings::GetSingleton().collectEnabled.load()) return true;
		auto* marker = PickupManager::GetHoldingMarker();
		if (!g_remainsUrn || !marker) {
			RE::DebugNotification("Picking up ashes needs BuryTakeBodies.esp enabled.");
			return true;
		}
		if (!PickupManager::HasFreeSlot()) {
			RE::DebugNotification("You can't carry any more.");
			return true;
		}
		RE::NiPointer<RE::TESObjectREFR> urn = marker->PlaceObjectAtMe(g_remainsUrn, true);
		if (!urn) return true;
		auto* actor = AshPileActor(pile);
		std::string name = DeadNameOf(actor ? static_cast<RE::TESObjectREFR*>(actor) : pile);
		urn->SetDisplayName(RE::BSFixedString("Ashes of " + (name.empty() ? std::string("the fallen") : name)), true);
		MakeUrnSmall(urn.get());
		const RE::FormID urnID = urn->GetFormID();

		// The pile (and any twin of it) leaves like a buried one; the actor behind it too.
		const auto twins = actor ? FindTwinAshPiles(pile, actor) : std::vector<RE::NiPointer<RE::TESObjectREFR>>{};
		pile->SetActivationBlocked(false);
		if (g_cleanupCell) {
			MoveRefTo(pile, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, pile->GetAngle());
		}
		pile->Disable();
		SendTwinsAway(twins);
		PlayTurnUndeadSound();
		if (!actor) {
			PickupManager::ExecuteCollect(urnID);
			logger::info("Pick Up: ash pile {:08X} (no actor) taken as urn {:08X}", a_refID, urnID);
			return true;
		}
		SendAshActorAway(actor);

		// Half a second later, with the actor gone from the loaded area: quest items
		// to the player, everything else into the urn, then the urn into the inventory.
		const RE::FormID actorID = actor->GetFormID();
		const std::uint32_t gen = g_generation.load();
		std::thread([actorID, urnID, gen]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([actorID, urnID, gen]() {
					if (g_generation.load() != gen) return;
					auto* a = RE::TESForm::LookupByID<RE::Actor>(actorID);
					auto* u = RE::TESForm::LookupByID<RE::TESObjectREFR>(urnID);
					if (!u) return;
					std::size_t types = 0;
					if (a) {
						MoveQuestItemsToPlayer(a);
						types = MoveAllItems(a, u);
					}
					PickupManager::ExecuteCollect(urnID);
					logger::info("Pick Up: ashes of {:08X} gathered into urn {:08X} ({} item types) and carried", actorID, urnID, types);
				});
			}
		}).detach();
		return true;
	}

	// Public wrapper for the crosshair sink; the test itself lives in the
	// anonymous namespace beside IsValidCorpse.
	bool IsAshPile(RE::TESObjectREFR* a_ref)
	{
		return ::IsAshPile(a_ref);
	}

	void ExecuteResurrect(RE::FormID a_refID)
	{
		if (!PFR::Settings::GetSingleton().resurrectEnabled.load()) return;
		auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
		if (!ref) return;
		auto* actor = ref->As<RE::Actor>();
		if (!actor || !actor->IsDead() || IsRemainsDraugr(actor)) return;

		const bool reset = PFR::Settings::GetSingleton().resurrectResetInventory.load();
		actor->Resurrect(reset, true);  // (resetInventory, attach3D)
		PlayTurnUndeadSound();

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("Resurrect: {:08X} reset={}", a_refID, reset);
		}
	}

	void ExecuteLayToRest(RE::FormID a_refID)
	{
		auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
		if (!IsValidCorpse(ref)) return;
		if (!g_cleanupCell) {
			logger::warn("LayToRest: cleanup cell not cached");
			return;
		}

		// NO explosion (removed).  Sound, teleport away, then quest items to the
		// player once the body is gone (see GiveQuestItemsLater).
		auto* actor = CorpseActor(ref);
		const bool questItems = HasQuestItems(actor);
		const auto twins = (actor && actor != ref) ? FindTwinAshPiles(ref, actor)
		                                           : std::vector<RE::NiPointer<RE::TESObjectREFR>>{};
		PlayTurnUndeadSound();
		MoveRefTo(ref, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, ref->GetAngle());
		SendTwinsAway(twins);
		if (questItems) {
			if (actor != ref) SendAshActorAway(actor);
			GiveQuestItemsLater(actor->GetFormID());
		}

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("LayToRest: {:08X} sent to cleanup cell", a_refID);
		}
	}

	void ExecuteBury(RE::FormID a_refID)
	{
		auto& settings = PFR::Settings::GetSingleton();

		// Bury is the HOLD half of the Lay/Bury key, so the shovel gate is enforced
		// here (the prompt itself always shows for the Lay tap).
		if (settings.buryRequiresShovel.load() && !PlayerHasShovel()) {
			RE::DebugNotification("You need a shovel to bury the dead.");
			return;
		}

		auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
		if (!IsValidCorpse(ref) && !IsUrn(ref)) return;   // a dug-up urn can be buried again

		if (!g_graveActivator) {
			RE::DebugNotification("Bury needs BuryTakeBodies.esp enabled.");
			return;
		}

		// Capture the deceased's name now (used as the first line of the grave).
		// An ash pile is named after the actor it came from, not "Ash Pile".
		RE::TESObjectREFR* named = CorpseActor(ref);
		if (!named) named = ref;
		std::string deadName;
		if (settings.useDisplayName.load()) {
			deadName = DeadNameOf(named);
		}
		if (deadName.empty()) {
			if (auto* base = named->GetBaseObject()) deadName = base->GetName();
		}

		// Block re-activation while the box is up.
		ref->SetActivationBlocked(true);

		// Hide the SkyPrompt buttons so they don't sit over the text box.
		PromptManager::Hide();

		// Without UIExtensions we can't show a text box — bury with name only.
		if (!g_hasUIExtensions) {
			// v1.4.3 (Nexus report): this fallback was SILENT — the player had no idea why
			// no box appeared. Say it once, right when it matters.
			RE::DebugNotification("Bury Take Bodies: install UIExtensions to write custom epitaphs.");
			std::string engraving = BuildEngraving(deadName, "");
			ref->SetActivationBlocked(false);
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([id = a_refID, engraving]() { DoBuryWithAnimation(id, engraving); });
			}
			return;
		}

		{
			std::scoped_lock lk(g_buryMutex);
			// An earlier bury that never finished keeps its body blocked - let it go.
			if (g_burying && g_buryBodyID != a_refID) {
				if (auto* old = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_buryBodyID)) {
					old->SetActivationBlocked(false);
				}
			}
			g_buryBoxOpened = false;
			g_burying = true;
			g_buryBodyID = a_refID;
			g_buryEditGraveID = 0;
			g_buryDeadName = deadName;
			g_buryMessage.clear();
			g_buryPrefill.clear();
			g_buryConfirmed = false;
		}

		OpenBuryTextEntry(false);
	}

	void ExecuteEditEpitaph(RE::FormID a_graveID)
	{
		auto* grave = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_graveID);
		if (!IsOurGrave(grave)) return;
		if (!g_hasUIExtensions) {
			RE::DebugNotification("Bury Take Bodies: install UIExtensions to edit epitaphs.");
			return;
		}

		const char* current = grave->GetDisplayFullName();
		std::string deadName, message;
		SplitEngraving(current ? std::string(current) : std::string(), deadName, message);

		PromptManager::Hide();

		{
			std::scoped_lock lk(g_buryMutex);
			// A bury whose box never closed keeps its body blocked - let it go.
			if (g_burying && g_buryBodyID) {
				if (auto* old = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_buryBodyID)) {
					old->SetActivationBlocked(false);
				}
			}
			g_buryBoxOpened = false;
			g_burying = true;
			g_buryBodyID = 0;
			g_buryEditGraveID = a_graveID;
			g_buryDeadName = deadName;
			g_buryMessage.clear();
			g_buryPrefill = message;
			g_buryConfirmed = false;
		}

		OpenBuryTextEntry(true);
	}

	bool IsOurGrave(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || !g_graveActivator) return false;
		return a_ref->GetBaseObject() == g_graveActivator;
	}

	void OnGameReset()
	{
		g_generation.fetch_add(1);
		std::scoped_lock lk(g_buryMutex);
		g_burying = false;
		g_buryBodyID = 0;
		g_buryEditGraveID = 0;
		g_buryDeadName.clear();
		g_buryMessage.clear();
		g_buryPrefill.clear();
		g_buryConfirmed = false;
		g_buryBoxOpened = false;
	}

	bool GraveHasRemains(RE::FormID a_graveID)
	{
		return PickupManager::HasBuriedBody(a_graveID);
	}

	void DestroyGrave(RE::FormID a_graveID)
	{
		auto* grave = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_graveID);
		if (!grave || grave->GetBaseObject() != g_graveActivator) return;  // only our graves

		// A unique NPC's grave: dig up their remains (a lootable container with
		// everything they carried) instead of just removing the headstone.
		if (PickupManager::HasBuriedBody(a_graveID)) {
			if (PFR::Settings::GetSingleton().buryRequiresShovel.load() && !PlayerHasShovel()) {
				RE::DebugNotification("You need a shovel to dig up the dead.");
				return;
			}
			PlayDigThen([a_graveID]() {
				auto* g = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_graveID);
				if (!g) return;
				const RE::FormID remainsID = PickupManager::TakeBuriedBody(a_graveID);
				if (auto* remains = RE::TESForm::LookupByID<RE::TESObjectREFR>(remainsID)) {
					if (g_remainsUrn && remains->GetBaseObject() == g_remainsUrn) {
						float heading = 0.0f;
						RE::NiPoint3 pos = SpotBeforeGrave(g, heading);
						// The canopic jar's origin is its centre (bounds z -10..10, half size
						// here); a little higher so it settles onto the ground with physics.
						pos.z += 8.0f;
						MakeUrnSmall(remains);   // still unloaded in the holding cell
						MoveRefTo(remains, g->GetParentCell(), g->GetWorldspace(), pos, RE::NiPoint3{ 0.0f, 0.0f, heading });
						RE::DebugNotification("You dig up a burial urn.");
						logger::info("Dig Up: urn {:08X} set down at grave {:08X}", remainsID, g->GetFormID());
					} else if (RaiseRemainsDraugr(remains, g)) {
						RE::DebugNotification("You dig up their remains.");
					} else {
						remains->MoveTo(g);   // no draugr record: the old skeleton container
						RE::DebugNotification("You dig up the remains.");
					}
				}
				PlayTurnUndeadSound();
				g->Disable();
				g->SetDelete(true);
			});
			return;
		}
		if (!PFR::Settings::GetSingleton().graveDestroyEnabled.load()) return;

		PlayTurnUndeadSound();
		grave->Disable();
		grave->SetDelete(true);

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("DestroyGrave: removed {:08X}", a_graveID);
		}
	}
}
