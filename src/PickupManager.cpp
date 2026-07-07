#include "pch.h"
#include "PickupManager.h"
#include "Settings.h"

#include <cstdint>
#include <mutex>

namespace
{
	// ------------------------------------------------------------------
	// The corpse-carry system.
	//
	// BuryTakeBodies.esp ships 50 distinct MiscObject "corpse" tokens plus one
	// holding-cell marker.  Distinct base forms are what let each carried body
	// keep its OWN name + weight in the inventory (a single shared form can't).
	//
	// On collect we teleport the real body into the (never-rendered) holding
	// cell and hand the player the matching token, renamed "<Name>'s body".
	// On drop we teleport the body back to the dropped token and free the slot.
	//
	// The token name/weight are runtime edits to the base form, which the game
	// does NOT persist — so we mirror every occupied slot into the SKSE cosave
	// and re-apply it on load.  That is the real fix for the original mod's
	// "name resets to Collected Corpse / weight 1 after reload" bug.
	// ------------------------------------------------------------------

	constexpr const char* kPlugin        = "BuryTakeBodies.esp";
	constexpr RE::FormID  kMarkerLocalID = 0x801;      // PFR_HoldingMarker
	constexpr RE::FormID  kFirstCorpse   = 0x802;      // PFR_Corpse01
	constexpr RE::FormID  kActorTypeNPC  = 0x00013794; // Skyrim.esm keyword

	constexpr std::uint32_t kCosaveID   = 'PFRC';
	constexpr std::uint32_t kRecordType = 'CRPS';
	constexpr std::uint32_t kVersion    = 1;

	struct Slot
	{
		RE::TESObjectMISC* misc = nullptr;
		RE::FormID         miscLocalID = 0;   // 0x802.. (stable cosave key)
		RE::BSFixedString  defaultName;       // "Corpse"
		float              defaultWeight = 1.0f;

		bool               occupied = false;
		RE::FormID         bodyID = 0;        // runtime FormID of stashed body
		std::string        name;              // "<Name>'s body"
		float              weight = 1.0f;
	};

	std::vector<Slot>   g_slots;
	RE::TESObjectREFR*  g_marker = nullptr;
	RE::BGSKeyword*     g_actorTypeNPC = nullptr;
	std::recursive_mutex g_mutex;

	void ClearAllSlots()
	{
		for (auto& s : g_slots) {
			s.occupied = false;
			s.bodyID = 0;
			s.name.clear();
			s.weight = s.defaultWeight;
			if (s.misc) {
				s.misc->fullName = s.defaultName;
				s.misc->weight = s.defaultWeight;
			}
		}
	}

	// ------------------------------------------------------------------
	// The dropped world ref is created a frame or two AFTER the container
	// event, and the event itself carries a null reference (confirmed in the
	// log: worldRef=no), so we can't get the dropped ref from the event. Find
	// it by scanning the PLAYER'S OWN CELL for a ref of the token's base form.
	//
	// IMPORTANT: we scan a single cell (ForEachReference), NOT the world grid
	// (TES::ForEachReferenceInRange).  The grid walk crashed when a drop
	// happened right after a save-load while exterior cells were still
	// streaming (torn grid -> access violation).  A dropped item lands in the
	// player's own cell, so a single-cell scan finds it and is main-thread safe.
	// ------------------------------------------------------------------
	RE::TESObjectREFR* FindDroppedToken(RE::FormID a_tokenBase)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return nullptr;
		auto* cell = player->GetParentCell();
		if (!cell) return nullptr;

		RE::TESObjectREFR* found = nullptr;
		cell->ForEachReference([&](RE::TESObjectREFR& a_ref) {
			auto* base = a_ref.GetBaseObject();
			if (base && base->GetFormID() == a_tokenBase &&
				!a_ref.IsMarkedForDeletion() && !a_ref.IsDisabled()) {
				found = &a_ref;
				return RE::BSContainer::ForEachResult::kStop;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return found;
	}

	// ------------------------------------------------------------------
	// Respawn a stashed body at the dropped token, bin the token, free the
	// slot.  Runs on the main thread; retries a few frames until the dropped
	// world ref has spawned.
	// ------------------------------------------------------------------
	void RespawnFromDrop(std::size_t a_slotIdx, RE::FormID a_tokenBase, RE::FormID a_bodyID, int a_attempt)
	{
		RE::TESObjectREFR* token = FindDroppedToken(a_tokenBase);
		if (!token && a_attempt < 8) {
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([a_slotIdx, a_tokenBase, a_bodyID, a_attempt]() {
					RespawnFromDrop(a_slotIdx, a_tokenBase, a_bodyID, a_attempt + 1);
				});
			}
			return;
		}

		auto* body = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_bodyID);
		if (body) {
			if (token) {
				body->MoveTo(token);  // exact drop spot
			} else if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				body->MoveTo(player); // token never found — drop at the player
			}
		}
		if (token) {
			token->Disable();
			token->SetDelete(true);
		}

		logger::info("Pickup: drop slot={} body={:08X} bodyFound={} token={} attempt={}",
			a_slotIdx, a_bodyID, body ? "yes" : "no", token ? "yes" : "no", a_attempt);

		std::scoped_lock lk(g_mutex);
		if (a_slotIdx < g_slots.size()) {
			Slot& s = g_slots[a_slotIdx];
			if (s.misc) {
				s.misc->fullName = s.defaultName;
				s.misc->weight = s.defaultWeight;
			}
			s.occupied = false;
			s.bodyID = 0;
			s.name.clear();
			s.weight = s.defaultWeight;
		}
	}

	// ------------------------------------------------------------------
	// Fires whenever an item changes container.  We only care about one of our
	// corpse tokens leaving the PLAYER to the WORLD (a drop) — putting it in a
	// chest leaves the body stashed, matching the original mod.
	// ------------------------------------------------------------------
	class DropSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
	{
	public:
		static DropSink& GetSingleton()
		{
			static DropSink instance;
			return instance;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
			RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
		{
			if (!a_event) return RE::BSEventNotifyControl::kContinue;

			// Is this one of our carried corpse tokens moving?
			std::size_t slotIdx = SIZE_MAX;
			RE::FormID  bodyID = 0;
			{
				std::scoped_lock lk(g_mutex);
				for (std::size_t i = 0; i < g_slots.size(); ++i) {
					auto& s = g_slots[i];
					if (s.occupied && s.misc && s.misc->GetFormID() == a_event->baseObj) {
						slotIdx = i;
						bodyID = s.bodyID;
						break;
					}
				}
			}
			if (slotIdx == SIZE_MAX) return RE::BSEventNotifyControl::kContinue;

			logger::info("DropSink: corpse token {:08X} moved old={:08X} new={:08X} slot={}",
				a_event->baseObj, a_event->oldContainer, a_event->newContainer, slotIdx);

			// Only a world DROP from the player restores the body. The event's
			// own `reference` is null here, so we find the dropped ref later.
			// Moving the token into a container (new != 0) leaves it stashed —
			// same behaviour as the original mod.
			if (a_event->oldContainer != 0x14) return RE::BSEventNotifyControl::kContinue;
			if (a_event->newContainer != 0) return RE::BSEventNotifyControl::kContinue;

			RE::FormID tokenBase = a_event->baseObj;
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([slotIdx, tokenBase, bodyID]() {
					RespawnFromDrop(slotIdx, tokenBase, bodyID, 0);
				});
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ------------------------------------------------------------------
	// Cosave callbacks.
	// ------------------------------------------------------------------
	void SaveCallback(SKSE::SerializationInterface* a_intfc)
	{
		std::scoped_lock lk(g_mutex);
		if (!a_intfc->OpenRecord(kRecordType, kVersion)) return;

		std::uint32_t count = 0;
		for (auto& s : g_slots) {
			if (s.occupied) ++count;
		}
		a_intfc->WriteRecordData(count);

		for (auto& s : g_slots) {
			if (!s.occupied) continue;
			a_intfc->WriteRecordData(s.miscLocalID);
			a_intfc->WriteRecordData(s.bodyID);
			a_intfc->WriteRecordData(s.weight);
			std::uint32_t len = static_cast<std::uint32_t>(s.name.size());
			a_intfc->WriteRecordData(len);
			if (len) a_intfc->WriteRecordData(s.name.data(), len);
		}
	}

	void LoadCallback(SKSE::SerializationInterface* a_intfc)
	{
		std::scoped_lock lk(g_mutex);
		ClearAllSlots();

		std::uint32_t type = 0, version = 0, length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != kRecordType) continue;

			std::uint32_t count = 0;
			a_intfc->ReadRecordData(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				RE::FormID    miscLocal = 0;
				RE::FormID    bodyOld = 0;
				float         w = 1.0f;
				std::uint32_t len = 0;
				a_intfc->ReadRecordData(miscLocal);
				a_intfc->ReadRecordData(bodyOld);
				a_intfc->ReadRecordData(w);
				a_intfc->ReadRecordData(len);
				std::string name;
				if (len) {
					name.resize(len);
					a_intfc->ReadRecordData(name.data(), len);
				}

				RE::FormID bodyNew = 0;
				if (!a_intfc->ResolveFormID(bodyOld, bodyNew)) bodyNew = 0;

				Slot* slot = nullptr;
				for (auto& s : g_slots) {
					if (s.miscLocalID == miscLocal) { slot = &s; break; }
				}
				if (!slot) continue;

				slot->occupied = true;
				slot->bodyID = bodyNew;
				slot->name = name;
				slot->weight = w;
				if (slot->misc) {                    // <-- the persistence fix
					slot->misc->fullName = RE::BSFixedString(name);
					slot->misc->weight = w;
				}
			}
		}
	}

	void RevertCallback(SKSE::SerializationInterface*)
	{
		std::scoped_lock lk(g_mutex);
		ClearAllSlots();
	}
}

namespace PickupManager
{
	void RegisterSerialization()
	{
		auto* ser = SKSE::GetSerializationInterface();
		if (!ser) return;
		ser->SetUniqueID(kCosaveID);
		ser->SetSaveCallback(SaveCallback);
		ser->SetLoadCallback(LoadCallback);
		ser->SetRevertCallback(RevertCallback);
	}

	void Init()
	{
		std::scoped_lock lk(g_mutex);
		g_slots.clear();

		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			logger::warn("Pickup: no data handler");
			return;
		}

		// Corpse tokens are contiguous from 0x802; stop at the first gap.
		for (RE::FormID local = kFirstCorpse; ; ++local) {
			auto* m = dh->LookupForm<RE::TESObjectMISC>(local, kPlugin);
			if (!m) break;
			Slot s;
			s.misc = m;
			s.miscLocalID = local;
			s.defaultName = m->GetFullName() ? m->GetFullName() : "Corpse";
			s.defaultWeight = m->weight;
			g_slots.push_back(std::move(s));
		}

		g_marker = dh->LookupForm<RE::TESObjectREFR>(kMarkerLocalID, kPlugin);
		g_actorTypeNPC = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNPC);

		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESContainerChangedEvent>(&DropSink::GetSingleton());
		}

		logger::info("Pickup: {} corpse slots, marker={}, ActorTypeNPC={}",
			g_slots.size(), g_marker != nullptr, g_actorTypeNPC != nullptr);

		if (g_slots.empty() || !g_marker) {
			logger::warn("Pickup: BuryTakeBodies.esp not found/loaded — pick-up disabled");
		}
	}

	bool CanCollect(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) return false;
		if (g_slots.empty() || !g_marker) return false;
		auto* actor = a_ref->As<RE::Actor>();
		if (!actor || !actor->IsDead()) return false;
		// Humanoid only (ActorTypeNPC).  Dremora carry this keyword, so unlike
		// the original mod they CAN be collected.
		if (g_actorTypeNPC && !actor->HasKeyword(g_actorTypeNPC)) return false;
		return true;
	}

	void ExecuteCollect(RE::FormID a_refID)
	{
		std::scoped_lock lk(g_mutex);

		auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
		auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
		if (!actor || !actor->IsDead()) return;

		if (!g_marker) {
			RE::DebugNotification("Corpse storage missing (BuryTakeBodies.esp not loaded).");
			return;
		}

		// Already carrying this exact body?
		for (auto& s : g_slots) {
			if (s.occupied && s.bodyID == a_refID) {
				RE::DebugNotification("You are already carrying this body.");
				return;
			}
		}

		Slot* slot = nullptr;
		for (auto& s : g_slots) {
			if (!s.occupied) { slot = &s; break; }
		}
		if (!slot) {
			RE::DebugNotification("You can't carry any more bodies.");
			return;
		}

		// Name (display name so Real Names / rename mods show correctly).
		std::string dispName;
		if (const char* dn = actor->GetDisplayFullName()) dispName = dn;
		if (dispName.empty()) {
			if (auto* base = actor->GetBaseObject()) dispName = base->GetName();
		}
		std::string itemName = dispName.empty() ? "Corpse" : (dispName + "'s body");

		// Weight = sex base (male 130 / female 100 / other 115) + carried gear.
		float base = 115.0f;
		if (auto* npc = actor->GetActorBase()) {
			switch (npc->GetSex()) {
			case RE::SEX::kMale:   base = 130.0f; break;
			case RE::SEX::kFemale: base = 100.0f; break;
			default: break;
			}
		}
		float weight = base + actor->GetWeightInContainer();

		slot->misc->fullName = RE::BSFixedString(itemName);
		slot->misc->weight = weight;

		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			player->AddObjectToContainer(slot->misc, nullptr, 1, nullptr);
		}
		actor->MoveTo(g_marker);

		slot->occupied = true;
		slot->bodyID = a_refID;
		slot->name = itemName;
		slot->weight = weight;

		RE::DebugNotification(("Picked up " + itemName + ".").c_str());

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("Pickup: collected {:08X} as '{}' (w={:.0f})", a_refID, itemName, weight);
		}
	}
}
