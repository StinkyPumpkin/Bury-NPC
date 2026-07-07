#include "pch.h"
#include "RespectManager.h"
#include "Settings.h"
#include "PromptManager.h"

#include <mutex>

namespace
{
	RE::TESObjectCELL*          g_cleanupCell = nullptr;
	RE::BGSSoundDescriptorForm* g_soundForm = nullptr;
	RE::TESBoundObject*         g_graveActivator = nullptr;
	bool                        g_hasUIExtensions = false;

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

	// ------------------------------------------------------------------
	// Move a reference to another cell (used to send the corpse to the
	// vanilla WIDeadBodyCleanupCell so it disappears).
	// ------------------------------------------------------------------
	void MoveRefTo(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_targetCell,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_rotation)
	{
		using func_t = void (*)(RE::TESObjectREFR*, const RE::ObjectRefHandle&,
			RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
		static REL::Relocation<func_t> func{ RELOCATION_ID(56227, 56626) };
		func(a_ref, RE::ObjectRefHandle{}, a_targetCell, nullptr, a_position, a_rotation);
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

	bool IsValidCorpse(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) return false;
		auto* actor = a_ref->As<RE::Actor>();
		if (actor && actor->IsDead()) return true;

		// Ash piles (turned-undead remains) — name contains "ash".
		if (auto* base = a_ref->GetBaseObject()) {
			std::string name = base->GetName();
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (name.find("ash") != std::string::npos) return true;
		}
		return false;
	}

	// ------------------------------------------------------------------
	// Place the inscribed gravestone at the body and remove the body.  Runs on
	// the main thread via the task queue.
	// ------------------------------------------------------------------
	void PlaceGraveAndBury(RE::FormID a_bodyID, std::string a_engraving)
	{
		auto* body = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_bodyID);
		if (!body) return;

		if (g_graveActivator) {
			RE::NiPointer<RE::TESObjectREFR> grave =
				body->PlaceObjectAtMe(g_graveActivator, false);
			if (grave) {
				grave->SetDisplayName(RE::BSFixedString(a_engraving), true);
			} else {
				logger::warn("Bury: PlaceObjectAtMe returned null for {:08X}", a_bodyID);
			}
		}

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
		if (!a_message.empty()) {
			engraving += " — ";
			engraving += a_message;
		}
		return engraving;
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
			task->AddTask([bodyID, engraving]() { PlaceGraveAndBury(bodyID, engraving); });
		}
	}

	// ------------------------------------------------------------------
	// Open the UIExtensions text-entry box (registered custom menu name
	// "textentrymenu") via the Papyrus VM.
	// ------------------------------------------------------------------
	void OpenBuryTextEntry()
	{
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
			if (a_event->eventName == "UITextEntryMenu_TextChanged") {
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
		}

		BuryDialogSink::GetSingleton().Register();

		logger::info("RespectManager: cleanupCell={} sound={} graveActivator={} UIExtensions={}",
			g_cleanupCell != nullptr, g_soundForm != nullptr, g_graveActivator != nullptr,
			g_hasUIExtensions);

		if (!g_graveActivator) {
			logger::warn("RespectManager: BuryTakeBodies.esp grave activator not found — "
				"enable BuryTakeBodies.esp for the Bury option");
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

		// NO explosion (removed).  Sound + teleport away.
		PlayTurnUndeadSound();
		MoveRefTo(ref, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, ref->GetAngle());

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("LayToRest: {:08X} sent to cleanup cell", a_refID);
		}
	}

	void ExecuteBury(RE::FormID a_refID)
	{
		auto& settings = PFR::Settings::GetSingleton();

		auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
		if (!IsValidCorpse(ref)) return;

		if (!g_graveActivator) {
			RE::DebugNotification("Bury needs BuryTakeBodies.esp enabled.");
			return;
		}

		// Capture the deceased's name now (used as the first line of the grave).
		std::string deadName;
		if (settings.useDisplayName.load()) {
			deadName = ref->GetDisplayFullName();
		}
		if (deadName.empty()) {
			if (auto* base = ref->GetBaseObject()) deadName = base->GetName();
		}

		// Block re-activation while the box is up.
		ref->SetActivationBlocked(true);

		// Hide the SkyPrompt buttons so they don't sit over the text box.
		PromptManager::Hide();

		// Without UIExtensions we can't show a text box — bury with name only.
		if (!g_hasUIExtensions) {
			std::string engraving = BuildEngraving(deadName, "");
			ref->SetActivationBlocked(false);
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([id = a_refID, engraving]() { PlaceGraveAndBury(id, engraving); });
			}
			return;
		}

		{
			std::scoped_lock lk(g_buryMutex);
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

	void DestroyGrave(RE::FormID a_graveID)
	{
		auto* grave = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_graveID);
		if (!grave || grave->GetBaseObject() != g_graveActivator) return;  // only our graves

		PlayTurnUndeadSound();
		grave->Disable();
		grave->SetDelete(true);

		if (PFR::Settings::GetSingleton().debug.load()) {
			logger::debug("DestroyGrave: removed {:08X}", a_graveID);
		}
	}
}
