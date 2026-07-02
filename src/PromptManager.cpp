#include "pch.h"
#include "PromptManager.h"
#include "Settings.h"
#include "RespectManager.h"

namespace
{
	constexpr SkyPromptAPI::EventID PROMPT_EVENT_ID = 47312;

	// actionIDs distinguish the two prompts inside ProcessEvent.
	constexpr SkyPromptAPI::ActionID ACTION_LAY = 0;
	constexpr SkyPromptAPI::ActionID ACTION_BURY = 1;

	SkyPromptAPI::ClientID    g_clientID = 0;
	std::atomic<bool>         g_showing{ false };
	std::atomic<RE::FormID>   g_currentRefID{ 0 };

	// One key-pair (keyboard, gamepad) per action.
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_layKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_buryKeys[2];

	struct RespectPromptSink : public SkyPromptAPI::PromptSink
	{
		SkyPromptAPI::Prompt m_prompts[2];

		RespectPromptSink()
		{
			// Lay to Rest — hold.
			m_prompts[0].text = "Lay to Rest";
			m_prompts[0].eventID = PROMPT_EVENT_ID;
			m_prompts[0].actionID = ACTION_LAY;
			m_prompts[0].type = SkyPromptAPI::PromptType::kHold;
			m_prompts[0].refid = 0;
			m_prompts[0].text_color = 0xFFFFFFFF;
			m_prompts[0].progress = 0.0f;

			// Bury with Gravestone — hold.
			m_prompts[1].text = "Bury with Gravestone";
			m_prompts[1].eventID = PROMPT_EVENT_ID;
			m_prompts[1].actionID = ACTION_BURY;
			m_prompts[1].type = SkyPromptAPI::PromptType::kHold;
			m_prompts[1].refid = 0;
			m_prompts[1].text_color = 0xFFFFFFFF;
			m_prompts[1].progress = 0.0f;
		}

		std::span<const SkyPromptAPI::Prompt> GetPrompts() const override
		{
			return std::span<const SkyPromptAPI::Prompt>(m_prompts, 2);
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
				} else {
					RespectManager::ExecuteLayToRest(refID);
				}
			});
		}
	};

	RespectPromptSink g_sink;

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

		g_sink.m_prompts[0].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_layKeys, 2);
		g_sink.m_prompts[1].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_buryKeys, 2);
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
	if (auto* crosshairSrc = SKSE::GetCrosshairRefEventSource()) {
		crosshairSrc->RemoveEventSink(&CrosshairSink::GetSingleton());
	}
}

void PromptManager::ShowForRef(RE::TESObjectREFR* a_ref)
{
	if (!a_ref || g_clientID == 0) return;

	RE::FormID refID = a_ref->GetFormID();
	g_currentRefID.store(refID);

	g_sink.m_prompts[0].refid = refID;
	g_sink.m_prompts[1].refid = refID;

	(void)SkyPromptAPI::SendPrompt(&g_sink, g_clientID);
	g_showing.store(true);
}

void PromptManager::Hide()
{
	if (g_showing.load()) {
		SkyPromptAPI::RemovePrompt(&g_sink, g_clientID);
		g_showing.store(false);
		g_currentRefID.store(0);
	}
}

void PromptManager::RefreshHotkeys()
{
	BuildButtonKeys();
}
