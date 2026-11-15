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

	struct ItemMove
	{
		RE::TESBoundObject* obj;
		RE::ExtraDataList*  xList;
		std::int32_t        count;
	};

	std::vector<ItemMove> CollectItems(RE::TESObjectREFR* a_from, bool a_questOnly)
	{
		std::vector<ItemMove> moves;
		if (!a_from) return moves;
		auto inv = a_from->GetInventory();
		for (auto& [obj, data] : inv) {
			auto& [count, entry] = data;
			if (!obj || count <= 0 || !entry) continue;
			if (a_questOnly && !entry->IsQuestObject()) continue;
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

	void MoveItems(RE::TESObjectREFR* a_from, RE::TESObjectREFR* a_to, const std::vector<ItemMove>& a_moves)
	{
		for (const auto& m : a_moves) {
			a_from->RemoveItem(m.obj, m.count, RE::ITEM_REMOVE_REASON::kStoreInContainer, m.xList, a_to);
		}
	}

	// Lay to Rest and Bury send the corpse away; anything a quest needs comes to
	// the player first so the quest can't get stuck.
	void GiveQuestItemsToPlayer(RE::TESObjectREFR* a_corpse)
	{
		auto* actor = CorpseActor(a_corpse);
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!actor || !player) return;
		auto moves = CollectItems(actor, true);
		if (moves.empty()) return;
		MoveItems(actor, player, moves);
		RE::DebugNotification("You took the quest items they carried.");
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
	bool StoreRemains(RE::TESObjectREFR* a_grave, RE::TESObjectREFR* a_body)
	{
		auto* actor = CorpseActor(a_body);
		auto* npc = actor ? actor->GetActorBase() : nullptr;
		auto* marker = PickupManager::GetHoldingMarker();
		if (!a_grave || !npc || !npc->IsUnique() || !g_remains || !marker) return false;

		RE::NiPointer<RE::TESObjectREFR> remains = marker->PlaceObjectAtMe(g_remains, true);
		if (!remains) return false;
		MoveItems(actor, remains.get(), CollectItems(actor, false));
		const char* name = actor->GetDisplayFullName();
		remains->SetDisplayName(RE::BSFixedString(std::string("Remains of ") + (name && *name ? name : "the fallen")), true);
		PickupManager::RegisterBuriedBody(a_grave->GetFormID(), remains->GetFormID());
		return true;
	}

	void PlaceGraveAndBury(RE::FormID a_bodyID, std::string a_engraving)
	{
		auto* body = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_bodyID);
		if (!body) return;

		bool stored = false;
		if (g_graveActivator) {
			RE::NiPointer<RE::TESObjectREFR> grave =
				body->PlaceObjectAtMe(g_graveActivator, true);   // persistent: a grave can hold remains
			if (grave) {
				StandGraveUpright(grave.get(), body);
				grave->SetDisplayName(RE::BSFixedString(a_engraving), true);
				stored = StoreRemains(grave.get(), body);
			} else {
				logger::warn("Bury: PlaceObjectAtMe returned null for {:08X}", a_bodyID);
			}
		}
		if (!stored) GiveQuestItemsToPlayer(body);

		PlayTurnUndeadSound();
		body->SetActivationBlocked(false);
		if (g_cleanupCell) {
			MoveRefTo(body, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, body->GetAngle());
		}
		body->Disable();

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("Bury: buried {:08X} — '{}'", a_bodyID, a_engraving);
		}
	}

	std::string BuildEngraving(const std::string& a_deadName, const std::string& a_message)
	{
		std::string engraving = a_deadName.empty() ? std::string("Here lies the fallen") : a_deadName;
		// v1.4.3: the NAME is engraved automatically — if the player typed the name again
		// (field report: grave read "Ivana — Ivana"), treat it as no message.
		auto iequals = [](const std::string& a, const std::string& b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
					return false;
			return true;
		};
		if (!a_message.empty() && !iequals(a_message, a_deadName)) {
			engraving += " — ";
			engraving += a_message;
		}
		return engraving;
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
		RE::FormID  bodyID;
		std::string deadName, message;
		bool        confirmed;
		{
			std::scoped_lock lk(g_buryMutex);
			if (!g_burying) return;
			bodyID = g_buryBodyID;
			deadName = g_buryDeadName;
			message = g_buryMessage;
			confirmed = g_buryConfirmed;
			g_burying = false;
			g_buryBodyID = 0;
			g_buryDeadName.clear();
			g_buryMessage.clear();
			g_buryConfirmed = false;
			g_buryBoxOpened = false;
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
	void OpenBuryTextEntry()
	{
		// v1.4.3 (user request): tell the player what the box is for — the NAME goes on
		// automatically, the box is only the epitaph.
		RE::DebugNotification("Write your epitaph — the name is engraved automatically.");

		// If the box never appears (UIExtensions installed but its menu failed),
		// the body would stay blocked from activation for good, and that block is
		// saved. After a few seconds without the box, bury with the name only.
		const std::uint32_t gen = g_generation.load();
		std::thread([gen]() {
			std::this_thread::sleep_for(std::chrono::seconds(4));
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([gen]() {
					if (g_generation.load() != gen) return;
					RE::FormID bodyID = 0;
					std::string deadName;
					{
						std::scoped_lock lk(g_buryMutex);
						if (!g_burying || g_buryBoxOpened) return;
						bodyID = g_buryBodyID;
						deadName = g_buryDeadName;
						g_burying = false;
						g_buryBodyID = 0;
						g_buryDeadName.clear();
						g_buryMessage.clear();
						g_buryConfirmed = false;
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
	void AddEpitaphTip(const std::string& a_deadName)
	{
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
			" — write an epitaph:";
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
				std::string deadName;
				bool active;
				{
					std::scoped_lock lk(g_buryMutex);
					active = g_burying;
					deadName = g_buryDeadName;
				}
				if (active) {
					SKSE::GetTaskInterface()->AddUITask([deadName]() { AddEpitaphTip(deadName); });
				}
			} else if (a_event->eventName == "UITextEntryMenu_TextChanged") {
				std::scoped_lock lk(g_buryMutex);
				if (g_burying) {
					g_buryMessage = a_event->strArg.c_str();
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
		}

		BuryDialogSink::GetSingleton().Register();

		logger::info("RespectManager: cleanupCell={} sound={} graveActivator={} remains={} UIExtensions={}",
			g_cleanupCell != nullptr, g_soundForm != nullptr, g_graveActivator != nullptr,
			g_remains != nullptr, g_hasUIExtensions);

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
		if (!actor || !actor->IsDead()) return;

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

		// NO explosion (removed).  Quest items to the player, sound, teleport away.
		GiveQuestItemsToPlayer(ref);
		PlayTurnUndeadSound();
		MoveRefTo(ref, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, ref->GetAngle());

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
		if (!IsValidCorpse(ref)) return;

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
			deadName = named->GetDisplayFullName();
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
			g_buryDeadName = deadName;
			g_buryMessage.clear();
			g_buryConfirmed = false;
		}

		OpenBuryTextEntry();
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
		g_buryDeadName.clear();
		g_buryMessage.clear();
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
					remains->MoveTo(g);
					RE::DebugNotification("You dig up the remains.");
				}
				PlayTurnUndeadSound();
				g->Disable();
				g->SetDelete(true);
			});
			return;
		}

		PlayTurnUndeadSound();
		grave->Disable();
		grave->SetDelete(true);

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("DestroyGrave: removed {:08X}", a_graveID);
		}
	}
}
