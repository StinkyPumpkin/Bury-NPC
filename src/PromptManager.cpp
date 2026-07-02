#include "pch.h"
#include "PromptManager.h"
#include "Settings.h"
#include "RespectManager.h"
#include "PickupManager.h"

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

	// One key-pair (keyboard, gamepad) per action.
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_layKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_buryKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_collectKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_graveKeys[2];

	struct RespectPromptSink : public SkyPromptAPI::PromptSink
	{
		SkyPromptAPI::Prompt m_prompts[3];
		// How many prompts to expose right now (2 = Lay+Bury, 3 = +Pick Up).
		std::atomic<std::uint8_t> m_promptCount{ 2 };

		RespectPromptSink()
		{
			// Lay to Rest — hold.
			m_prompts[0].text = "Lay to Rest";
			m_prompts[0].eventID = EVENT_LAY;
			m_prompts[0].actionID = ACTION_LAY;
			m_prompts[0].type = SkyPromptAPI::PromptType::kHold;
			m_prompts[0].refid = 0;
			m_prompts[0].text_color = 0xFFFFFFFF;
			m_prompts[0].progress = 0.0f;

			// Bury with Gravestone — hold.
			m_prompts[1].text = "Bury with Gravestone";
			m_prompts[1].eventID = EVENT_BURY;
			m_prompts[1].actionID = ACTION_BURY;
			m_prompts[1].type = SkyPromptAPI::PromptType::kHold;
			m_prompts[1].refid = 0;
			m_prompts[1].text_color = 0xFFFFFFFF;
			m_prompts[1].progress = 0.0f;

			// Pick Up Body — hold (humanoid corpses only).
			m_prompts[2].text = "Pick Up Body";
			m_prompts[2].eventID = EVENT_COLLECT;
			m_prompts[2].actionID = ACTION_COLLECT;
			m_prompts[2].type = SkyPromptAPI::PromptType::kHold;
			m_prompts[2].refid = 0;
			m_prompts[2].text_color = 0xFFFFFFFF;
			m_prompts[2].progress = 0.0f;
		}

		std::span<const SkyPromptAPI::Prompt> GetPrompts() const override
		{
			return std::span<const SkyPromptAPI::Prompt>(m_prompts, m_promptCount.load());
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

	// Tracks the reveal modifier: a fresh modifier press while looking at a
	// grave reveals the Destroy Grave prompt; releasing hides it.  (Requiring a
	// fresh press means walking up while already holding the key — e.g.
	// sprinting — does not surprise-reveal it.)
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
			const auto modKey = static_cast<std::uint32_t>(
				PFR::Settings::GetSingleton().graveDestroyModifier.load());

			for (auto* e = *a_events; e; e = e->next) {
				if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
				auto* btn = e->AsButtonEvent();
				if (!btn || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
				if (btn->GetIDCode() != modKey) continue;

				if (btn->IsDown()) {
					if (g_currentGraveID.load() != 0) SendGravePrompt();
				} else if (btn->IsUp()) {
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

		g_sink.m_prompts[0].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_layKeys, 2);
		g_sink.m_prompts[1].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_buryKeys, 2);
		g_sink.m_prompts[2].button_key =
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

	if (auto* crosshairSrc = SKSE::GetCrosshairRefEventSource()) {
		crosshairSrc->AddEventSink(&CrosshairSink::GetSingleton());
	}
	if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
		idm->AddEventSink(&ModifierInputSink::GetSingleton());
	}

	(void)SkyPromptAPI::RequestTheme(g_clientID, "PressFtoPayRespects");
	logger::info("PromptManager: init clientID={} (Lay=kb{} / Bury=kb{})",
		g_clientID,
		PFR::Settings::GetSingleton().layKeyboard.load(),
		PFR::Settings::GetSingleton().buryKeyboard.load());
}

void PromptManager::Shutdown()
{
	if (g_showing.load()) {
		SkyPromptAPI::RemovePrompt(&g_sink, g_clientID);
		g_showing.store(false);
	}
	RemoveGravePrompt();
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
	g_currentRefID.store(refID);

	// Show the Pick Up prompt only on dead humanoids, and only if enabled.
	const bool canCollect = PFR::Settings::GetSingleton().collectEnabled.load() &&
		PickupManager::CanCollect(a_ref);
	const std::uint8_t count = canCollect ? 3 : 2;

	for (std::uint8_t i = 0; i < count; ++i) {
		g_sink.m_prompts[i].refid = refID;
	}
	g_sink.m_promptCount.store(count);

	(void)SkyPromptAPI::SendPrompt(&g_sink, g_clientID);
	g_showing.store(true);
}

void PromptManager::ShowGraveForRef(RE::TESObjectREFR* a_ref)
{
	if (!a_ref || g_clientID == 0) return;
	if (!PFR::Settings::GetSingleton().graveDestroyEnabled.load()) return;

	// Grave and body prompts are mutually exclusive.
	if (g_showing.load()) {
		SkyPromptAPI::RemovePrompt(&g_sink, g_clientID);
		g_showing.store(false);
		g_currentRefID.store(0);
	}

	// Just note which grave we're on — the Destroy Grave prompt itself only
	// appears when the reveal modifier is pressed (ModifierInputSink), so the
	// epitaph stays clean until the player deliberately holds it.
	g_currentGraveID.store(a_ref->GetFormID());
}

void PromptManager::Hide()
{
	if (g_showing.load()) {
		SkyPromptAPI::RemovePrompt(&g_sink, g_clientID);
		g_showing.store(false);
		g_currentRefID.store(0);
	}
	g_currentGraveID.store(0);
	RemoveGravePrompt();
}

void PromptManager::RefreshHotkeys()
{
	BuildButtonKeys();
}
