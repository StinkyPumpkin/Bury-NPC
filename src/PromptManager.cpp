#include "pch.h"
#include "PromptManager.h"
#include "Settings.h"
#include "RespectManager.h"
#include "PickupManager.h"
#include "QuickLootAPI.h"

namespace
{
	// SkyPrompt groups prompts that share an eventID into ONE row (they overlap
	// at the same slot). Distinct eventIDs put each prompt on its own row so all
	// three stack and stay visible. actionID still distinguishes them in
	// ProcessEvent.
	constexpr SkyPromptAPI::EventID EVENT_LAY = 47312;
	constexpr SkyPromptAPI::EventID EVENT_BURY = 47313;
	constexpr SkyPromptAPI::EventID EVENT_COLLECT = 47314;
	constexpr SkyPromptAPI::EventID EVENT_DESTROY = 47315;

	constexpr SkyPromptAPI::ActionID ACTION_LAY = 0;
	constexpr SkyPromptAPI::ActionID ACTION_BURY = 1;
	constexpr SkyPromptAPI::ActionID ACTION_COLLECT = 2;
	constexpr SkyPromptAPI::ActionID ACTION_DESTROY = 3;

	SkyPromptAPI::ClientID    g_clientID = 0;
	std::atomic<bool>         g_showing{ false };
	std::atomic<RE::FormID>   g_currentRefID{ 0 };

	// Grave (Destroy Grave) prompt state — separate sink, revealed by a
	// held modifier so it never clutters the epitaph.
	std::atomic<RE::FormID>   g_currentGraveID{ 0 };
	std::atomic<bool>         g_graveShowing{ false };

	// Reveal modifier (Shift) currently held, and whether we currently owe
	// QuickLoot an EnableLootMenu() to undo a DisableLootMenu().
	std::atomic<bool>         g_modifierHeld{ false };
	std::atomic<bool>         g_lootDisabled{ false };
	bool                      g_hasQuickLoot = false;

	// One key-pair (keyboard, gamepad) per action.
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_layKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_buryKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_collectKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_graveKeys[2];

	// ------------------------------------------------------------------
	// QuickLoot IE integration — hide its loot menu while our prompts show.
	// (The public API is the static class QuickLoot::API::QuickLootAPI.)
	// ------------------------------------------------------------------
	using QLApi = QuickLoot::API::QuickLootAPI;

	void DisableQuickLoot()
	{
		if (g_hasQuickLoot && !g_lootDisabled.exchange(true)) {
			QLApi::DisableLootMenu();
		}
	}

	void EnableQuickLoot()
	{
		if (g_hasQuickLoot && g_lootDisabled.exchange(false)) {
			QLApi::EnableLootMenu();
		}
	}

	// Backstop: refuse to open the loot menu while we've suppressed it (covers a
	// re-open race between DisableLootMenu and the crosshair re-evaluating).
	void OnOpeningLootMenu(QuickLoot::API::Events::OpeningLootMenuEvent* a_event)
	{
		if (a_event && g_lootDisabled.load()) {
			a_event->result = QuickLoot::API::Events::HandleResult::kStop;
		}
	}

	struct RespectPromptSink : public SkyPromptAPI::PromptSink
	{
		// Fixed templates (identity + keys). 0 = Lay, 1 = Bury, 2 = Pick Up.
		SkyPromptAPI::Prompt m_templates[3];
		// The active subset, packed contiguously (Bury drops out without a
		// shovel, Pick Up without a humanoid corpse), rebuilt each show.
		SkyPromptAPI::Prompt m_visible[3];
		std::atomic<std::uint8_t> m_promptCount{ 1 };

		RespectPromptSink()
		{
			// Lay to Rest — hold.
			m_templates[0].text = "Lay to Rest";
			m_templates[0].eventID = EVENT_LAY;
			m_templates[0].actionID = ACTION_LAY;
			m_templates[0].type = SkyPromptAPI::PromptType::kHold;
			m_templates[0].refid = 0;
			m_templates[0].text_color = 0xFFFFFFFF;
			m_templates[0].progress = 0.0f;

			// Bury with Gravestone — hold.
			m_templates[1].text = "Bury with Gravestone";
			m_templates[1].eventID = EVENT_BURY;
			m_templates[1].actionID = ACTION_BURY;
			m_templates[1].type = SkyPromptAPI::PromptType::kHold;
			m_templates[1].refid = 0;
			m_templates[1].text_color = 0xFFFFFFFF;
			m_templates[1].progress = 0.0f;

			// Pick Up Body — hold (humanoid corpses only).
			m_templates[2].text = "Pick Up Body";
			m_templates[2].eventID = EVENT_COLLECT;
			m_templates[2].actionID = ACTION_COLLECT;
			m_templates[2].type = SkyPromptAPI::PromptType::kHold;
			m_templates[2].refid = 0;
			m_templates[2].text_color = 0xFFFFFFFF;
			m_templates[2].progress = 0.0f;
		}

		// Pack the active prompts into m_visible in a stable order.
		void BuildVisible(RE::FormID a_refID, bool a_wantBury, bool a_wantCollect)
		{
			std::uint8_t n = 0;
			m_visible[n] = m_templates[0];  // Lay always
			m_visible[n].refid = a_refID;
			++n;
			if (a_wantBury) {
				m_visible[n] = m_templates[1];
				m_visible[n].refid = a_refID;
				++n;
			}
			if (a_wantCollect) {
				m_visible[n] = m_templates[2];
				m_visible[n].refid = a_refID;
				++n;
			}
			m_promptCount.store(n);
		}

		std::span<const SkyPromptAPI::Prompt> GetPrompts() const override
		{
			return std::span<const SkyPromptAPI::Prompt>(m_visible, m_promptCount.load());
		}

		void ProcessEvent(SkyPromptAPI::PromptEvent a_event) const override
		{
			if (a_event.type != SkyPromptAPI::PromptEventType::kAccepted) {
				return;
			}
			RE::FormID refID = g_currentRefID.load();
			if (refID == 0) return;

			const auto action = a_event.prompt.actionID;
			SKSE::GetTaskInterface()->AddTask([refID, action]() {
				if (action == ACTION_BURY) {
					RespectManager::ExecuteBury(refID);
				} else if (action == ACTION_COLLECT) {
					PickupManager::ExecuteCollect(refID);
				} else {
					RespectManager::ExecuteLayToRest(refID);
				}
			});
		}
	};

	RespectPromptSink g_sink;

	// Send / hide the body prompt set, keeping QuickLoot in sync.
	void SendBodyPrompt(bool a_disableLoot)
	{
		if (g_clientID == 0) return;
		if (!g_showing.load()) {
			(void)SkyPromptAPI::SendPrompt(&g_sink, g_clientID);
			g_showing.store(true);
		}
		if (a_disableLoot) DisableQuickLoot();
	}

	void HideBodyPrompt()
	{
		if (g_showing.load()) {
			SkyPromptAPI::RemovePrompt(&g_sink, g_clientID);
			g_showing.store(false);
		}
		EnableQuickLoot();
	}

	// ------------------------------------------------------------------
	// Destroy Grave — a one-prompt sink shown only while the modifier is held
	// on one of our graves.
	// ------------------------------------------------------------------
	struct GravePromptSink : public SkyPromptAPI::PromptSink
	{
		SkyPromptAPI::Prompt m_prompt;

		GravePromptSink()
		{
			m_prompt.text = "Destroy Grave";
			m_prompt.eventID = EVENT_DESTROY;
			m_prompt.actionID = ACTION_DESTROY;
			m_prompt.type = SkyPromptAPI::PromptType::kHold;
			m_prompt.refid = 0;
			m_prompt.text_color = 0xFFFFFFFF;
			m_prompt.progress = 0.0f;
		}

		std::span<const SkyPromptAPI::Prompt> GetPrompts() const override
		{
			return std::span<const SkyPromptAPI::Prompt>(&m_prompt, 1);
		}

		void ProcessEvent(SkyPromptAPI::PromptEvent a_event) const override
		{
			if (a_event.type != SkyPromptAPI::PromptEventType::kAccepted) return;
			RE::FormID graveID = g_currentGraveID.load();
			if (graveID == 0) return;
			SKSE::GetTaskInterface()->AddTask([graveID]() {
				RespectManager::DestroyGrave(graveID);
			});
		}
	};

	GravePromptSink g_graveSink;

	void SendGravePrompt()
	{
		if (g_clientID == 0) return;
		if (!PFR::Settings::GetSingleton().graveDestroyEnabled.load()) return;
		RE::FormID graveID = g_currentGraveID.load();
		if (graveID == 0) return;
		// refid 0 = not anchored to the grave's on-screen position (a grave sits
		// on the ground, which would drag the prompt to the bottom of the
		// screen / off the left edge).  With 0 it renders at the theme's fixed
		// centre.  We track the target grave in g_currentGraveID, not refid.
		g_graveSink.m_prompt.refid = 0;
		(void)SkyPromptAPI::SendPrompt(&g_graveSink, g_clientID);
		g_graveShowing.store(true);
	}

	void RemoveGravePrompt()
	{
		if (g_graveShowing.load()) {
			SkyPromptAPI::RemovePrompt(&g_graveSink, g_clientID);
			g_graveShowing.store(false);
		}
	}

	// Tracks the reveal modifier.  While it is held it reveals whichever prompt
	// set matches the current crosshair target: the Lay/Bury/PickUp buttons on a
	// corpse (and hides QuickLoot), or the Destroy Grave prompt on one of our
	// graves.  Releasing hides them again.
	class ModifierInputSink : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static ModifierInputSink& GetSingleton()
		{
			static ModifierInputSink instance;
			return instance;
		}

		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
			RE::BSTEventSource<RE::InputEvent*>*) override
		{
			if (!a_events) return RE::BSEventNotifyControl::kContinue;
			auto& s = PFR::Settings::GetSingleton();
			const auto modKey = static_cast<std::uint32_t>(s.graveDestroyModifier.load());

			for (auto* e = *a_events; e; e = e->next) {
				if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
				auto* btn = e->AsButtonEvent();
				if (!btn || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
				if (btn->GetIDCode() != modKey) continue;

				if (btn->IsDown()) {
					g_modifierHeld.store(true);
					if (g_currentRefID.load() != 0 && s.shiftGatesPrompts.load()) {
						SendBodyPrompt(true);
					} else if (g_currentGraveID.load() != 0) {
						SendGravePrompt();
					}
				} else if (btn->IsUp()) {
					g_modifierHeld.store(false);
					if (s.shiftGatesPrompts.load()) HideBodyPrompt();
					RemoveGravePrompt();
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void BuildButtonKeys()
	{
		auto& s = PFR::Settings::GetSingleton();
		g_layKeys[0] = { RE::INPUT_DEVICE::kKeyboard,
			static_cast<SkyPromptAPI::ButtonID>(s.layKeyboard.load()) };
		g_layKeys[1] = { RE::INPUT_DEVICE::kGamepad,
			static_cast<SkyPromptAPI::ButtonID>(s.layGamepad.load()) };
		g_buryKeys[0] = { RE::INPUT_DEVICE::kKeyboard,
			static_cast<SkyPromptAPI::ButtonID>(s.buryKeyboard.load()) };
		g_buryKeys[1] = { RE::INPUT_DEVICE::kGamepad,
			static_cast<SkyPromptAPI::ButtonID>(s.buryGamepad.load()) };
		g_collectKeys[0] = { RE::INPUT_DEVICE::kKeyboard,
			static_cast<SkyPromptAPI::ButtonID>(s.collectKeyboard.load()) };
		g_collectKeys[1] = { RE::INPUT_DEVICE::kGamepad,
			static_cast<SkyPromptAPI::ButtonID>(s.collectGamepad.load()) };

		g_sink.m_templates[0].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_layKeys, 2);
		g_sink.m_templates[1].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_buryKeys, 2);
		g_sink.m_templates[2].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_collectKeys, 2);

		// The Destroy Grave prompt fills on the execute key (held while the
		// reveal modifier is down).
		g_graveKeys[0] = { RE::INPUT_DEVICE::kKeyboard,
			static_cast<SkyPromptAPI::ButtonID>(s.graveDestroyKey.load()) };
		g_graveKeys[1] = { RE::INPUT_DEVICE::kGamepad,
			static_cast<SkyPromptAPI::ButtonID>(-1) };
		g_graveSink.m_prompt.button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_graveKeys, 2);
	}

	class CrosshairSink : public RE::BSTEventSink<SKSE::CrosshairRefEvent>
	{
	public:
		static CrosshairSink& GetSingleton()
		{
			static CrosshairSink instance;
			return instance;
		}

		RE::BSEventNotifyControl ProcessEvent(const SKSE::CrosshairRefEvent* a_event,
			RE::BSTEventSource<SKSE::CrosshairRefEvent>*) override
		{
			if (!a_event) return RE::BSEventNotifyControl::kContinue;

			auto ref = a_event->crosshairRef;
			if (ref) {
				auto* actor = ref->As<RE::Actor>();
				if (actor && actor->IsDead()) {
					PromptManager::ShowForRef(ref.get());
					return RE::BSEventNotifyControl::kContinue;
				}
				if (RespectManager::IsOurGrave(ref.get())) {
					PromptManager::ShowGraveForRef(ref.get());
					return RE::BSEventNotifyControl::kContinue;
				}
				if (auto* base = ref->GetBaseObject()) {
					std::string name = base->GetName();
					std::transform(name.begin(), name.end(), name.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (!name.empty() && name.find("ash") != std::string::npos) {
						PromptManager::ShowForRef(ref.get());
						return RE::BSEventNotifyControl::kContinue;
					}
				}
			}

			PromptManager::Hide();
			return RE::BSEventNotifyControl::kContinue;
		}
	};
}

void PromptManager::Init()
{
	g_clientID = SkyPromptAPI::RequestClientID();
	if (g_clientID == 0) {
		logger::warn("PromptManager: SkyPrompt not available, prompts disabled");
		return;
	}

	BuildButtonKeys();

	// Connect to QuickLoot IE (optional) so we can hide its loot menu while the
	// Shift-revealed action prompts are up.
	// Require only v20 — that's the interface DisableLootMenu/EnableLootMenu use.
	// (Defaulting to kLatest/v21 makes Init() return false on a v20-only server
	// like QuickLoot IE 3.4, even though the loot-menu toggle is available.)
	g_hasQuickLoot = QLApi::Init("BuryTakeBodies", QuickLoot::API::ApiVersion::kV20);
	if (g_hasQuickLoot) {
		QLApi::RegisterOpeningLootMenuHandler(OnOpeningLootMenu);
	}
	logger::info("PromptManager: QuickLoot API {} (shiftGates={})",
		g_hasQuickLoot ? "connected" : "not found",
		PFR::Settings::GetSingleton().shiftGatesPrompts.load());

	if (auto* crosshairSrc = SKSE::GetCrosshairRefEventSource()) {
		crosshairSrc->AddEventSink(&CrosshairSink::GetSingleton());
	}
	if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
		idm->AddEventSink(&ModifierInputSink::GetSingleton());
	}

	(void)SkyPromptAPI::RequestTheme(g_clientID, "BuryTakeBodies");
	logger::info("PromptManager: init clientID={} (Lay=kb{} / Bury=kb{}) shiftGated={}",
		g_clientID,
		PFR::Settings::GetSingleton().layKeyboard.load(),
		PFR::Settings::GetSingleton().buryKeyboard.load(),
		PFR::Settings::GetSingleton().shiftGatesPrompts.load());
}

void PromptManager::Shutdown()
{
	HideBodyPrompt();
	g_currentRefID.store(0);
	RemoveGravePrompt();
	EnableQuickLoot();
	if (auto* crosshairSrc = SKSE::GetCrosshairRefEventSource()) {
		crosshairSrc->RemoveEventSink(&CrosshairSink::GetSingleton());
	}
	if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
		idm->RemoveEventSink(&ModifierInputSink::GetSingleton());
	}
}

void PromptManager::ShowForRef(RE::TESObjectREFR* a_ref)
{
	if (!a_ref || g_clientID == 0) return;

	// Body and grave prompts are mutually exclusive.
	g_currentGraveID.store(0);
	RemoveGravePrompt();

	RE::FormID refID = a_ref->GetFormID();

	// If the crosshair moved to a different corpse, drop the old prompt set so
	// we can rebuild it (Bury/PickUp availability may differ).
	if (g_currentRefID.load() != refID) {
		HideBodyPrompt();
	}
	g_currentRefID.store(refID);

	auto& s = PFR::Settings::GetSingleton();

	// Bury needs a shovel (unless disabled); Pick Up needs a humanoid corpse.
	const bool wantBury = !s.buryRequiresShovel.load() || RespectManager::PlayerHasShovel();
	const bool wantCollect = s.collectEnabled.load() && PickupManager::CanCollect(a_ref);
	g_sink.BuildVisible(refID, wantBury, wantCollect);

	if (!s.shiftGatesPrompts.load()) {
		// Legacy behaviour: always show the prompts, leave QuickLoot alone.
		SendBodyPrompt(false);
	} else if (g_modifierHeld.load()) {
		// Modifier already held as we look at the corpse — reveal immediately.
		SendBodyPrompt(true);
	}
	// Otherwise the prompts stay hidden until the reveal modifier is pressed
	// (ModifierInputSink), so QuickLoot shows by default.
}

void PromptManager::ShowGraveForRef(RE::TESObjectREFR* a_ref)
{
	if (!a_ref || g_clientID == 0) return;
	if (!PFR::Settings::GetSingleton().graveDestroyEnabled.load()) return;

	// Grave and body prompts are mutually exclusive.
	HideBodyPrompt();
	g_currentRefID.store(0);

	// Just note which grave we're on — the Destroy Grave prompt itself only
	// appears when the reveal modifier is pressed (ModifierInputSink), so the
	// epitaph stays clean until the player deliberately holds it.
	g_currentGraveID.store(a_ref->GetFormID());
}

void PromptManager::Hide()
{
	HideBodyPrompt();
	g_currentRefID.store(0);
	g_currentGraveID.store(0);
	RemoveGravePrompt();
}

void PromptManager::RefreshHotkeys()
{
	BuildButtonKeys();
}
