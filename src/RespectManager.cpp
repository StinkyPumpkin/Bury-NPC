#include "pch.h"
#include "RespectManager.h"
#include "Settings.h"

namespace
{
	RE::TESObjectCELL*          g_cleanupCell = nullptr;
	RE::BGSSoundDescriptorForm* g_soundForm = nullptr;
	RE::TESBoundObject*         g_graveActivator = nullptr;

	// ------------------------------------------------------------------
	// Small deterministic-ish RNG for picking a memorial line.  Runs on the
	// main thread at bury time; std::rand is fine here.
	// ------------------------------------------------------------------
	int PickIndex(int a_count)
	{
		if (a_count <= 1) return 0;
		return std::rand() % a_count;
	}

	// Memorials copied verbatim from Pay Your Respects (WW42PYRBuryQuestScript).
	const char* const kFriend[] = {
		"Your sacrifice will never go unforgotten.",
		"True friends never really die.",
		"To live in the hearts of those we love is not to die.",
		"Wishing we had just a bit more time.",
	};
	const char* const kStranger[] = {
		"Rest in peace.",
		"Unknown to me, but not unknown to the Gods.",
		"May flowers grow here.",
		"A stranger in life, a friend to the Gods.",
	};
	const char* const kFoe[] = {
		"Well fought.",
		"By Akatosh, here you lie.",
		"Fallen in glorious battle.",
		"Kept their honor to the bitter end.",
	};

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

	// ------------------------------------------------------------------
	// Message-box callback wrapper.
	// ------------------------------------------------------------------
	class MessageCallback : public RE::IMessageBoxCallback
	{
	public:
		explicit MessageCallback(std::function<void(std::uint32_t)> a_fn) :
			_fn(std::move(a_fn)) {}
		~MessageCallback() override = default;

		void Run(RE::IMessageBoxCallback::Message a_msg) override
		{
			_fn(static_cast<std::uint32_t>(a_msg));
		}

	private:
		std::function<void(std::uint32_t)> _fn;
	};

	void ShowCategoryMessage(std::function<void(std::uint32_t)> a_cb)
	{
		auto* factoryManager = RE::MessageDataFactoryManager::GetSingleton();
		auto* strHolder = RE::InterfaceStrings::GetSingleton();
		if (!factoryManager || !strHolder) {
			a_cb(4);  // cancel
			return;
		}
		auto* factory = factoryManager->GetCreator<RE::MessageBoxData>(
			strHolder->messageBoxData);
		if (!factory) {
			a_cb(4);
			return;
		}
		auto* mb = factory->Create();
		if (!mb) {
			a_cb(4);
			return;
		}

		mb->callback = RE::make_smart<MessageCallback>(std::move(a_cb));
		mb->bodyText = "How would you like to remember them?";
		mb->buttonText.push_back("Friend");     // 0
		mb->buttonText.push_back("Stranger");   // 1
		mb->buttonText.push_back("Foe");        // 2
		mb->buttonText.push_back("Cancel");     // 3
		mb->QueueMessage();
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
}

namespace RespectManager
{
	void OnDataLoaded()
	{
		g_cleanupCell = RE::TESForm::LookupByEditorID<RE::TESObjectCELL>("WIDeadBodyCleanupCell");
		g_soundForm = RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(0x00057C65);

		// Grave activator now ships in PressFCorpses.esp (PFR_GraveActivator,
		// 0x834) — no Pay Your Respects dependency.
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (dh) {
			g_graveActivator = dh->LookupForm<RE::TESObjectACTI>(0x000834, "PressFCorpses.esp");
		}

		logger::info("RespectManager: cleanupCell={} sound={} graveActivator={}",
			g_cleanupCell != nullptr, g_soundForm != nullptr, g_graveActivator != nullptr);

		if (!g_graveActivator) {
			logger::warn("RespectManager: PressFCorpses.esp grave activator not found — "
				"enable PressFCorpses.esp for the Bury option");
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
			RE::DebugNotification("Bury needs Pay Your Respects installed.");
			return;
		}

		// Capture the engraving name NOW (ref may not survive the async box).
		std::string deadName;
		if (settings.useDisplayName.load()) {
			deadName = ref->GetDisplayFullName();
		}
		if (deadName.empty()) {
			if (auto* base = ref->GetBaseObject()) deadName = base->GetName();
		}

		std::string playerLine;
		if (settings.includePlayerName.load()) {
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				const char* pn = player->GetDisplayFullName();
				if (pn && pn[0]) {
					playerLine = std::string(" (buried by ") + pn + ")";
				}
			}
		}

		// Block re-activation while the menu is up.
		ref->SetActivationBlocked(true);

		ShowCategoryMessage([a_refID, deadName, playerLine](std::uint32_t a_button) {
			auto* body = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
			if (!body) return;

			if (a_button == 3 /*Cancel*/) {
				body->SetActivationBlocked(false);
				return;
			}

			const char* const* pool = kStranger;
			int poolSize = static_cast<int>(std::size(kStranger));
			if (a_button == 0) { pool = kFriend;  poolSize = static_cast<int>(std::size(kFriend)); }
			else if (a_button == 2) { pool = kFoe; poolSize = static_cast<int>(std::size(kFoe)); }

			// One-line inscription shown when you look at the grave.
			std::string engraving = deadName.empty() ? std::string("Here lies the fallen") : deadName;
			engraving += " — ";
			engraving += pool[PickIndex(poolSize)];
			engraving += playerLine;

			// Place the grave at the body, inscribe it (SetDisplayName persists
			// on the ref via extra data), remove the body.  No Papyrus / no
			// Pay Your Respects dependency.
			RE::NiPointer<RE::TESObjectREFR> grave =
				body->PlaceObjectAtMe(g_graveActivator, false);
			if (grave) {
				grave->SetDisplayName(RE::BSFixedString(engraving), true);
			} else {
				logger::warn("Bury: PlaceObjectAtMe returned null for {:08X}", a_refID);
			}

			PlayTurnUndeadSound();
			body->SetActivationBlocked(false);
			if (g_cleanupCell) {
				MoveRefTo(body, g_cleanupCell, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, body->GetAngle());
			}
			body->Disable();

			if (PFR::Settings::GetSingleton().debug.load()) {
				logger::debug("Bury: buried {:08X} as '{}'", a_refID, deadName);
			}
		});
	}
}
