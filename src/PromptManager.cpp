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
	// Two PAIRED prompts, each a kHold prompt. SkyPrompt sends the sink kDown on
	// press, kAccepted only once the hold bar fills, and kUp on release. A quick
	// TAP releases before the bar fills → kUp with NO kAccepted = the primary
	// action; a HOLD fills the bar → kAccepted = the secondary action. (Confirmed
	// against SkyPrompt src: Hooks.cpp ProcessInput + Renderer.cpp UpdateProgressCircle.)
	//   Pair A (default F): tap = Lay to Rest, hold = Bury with Gravestone
	//   Pair B (default E): tap = Resurrect,   hold = Take Body
	constexpr SkyPromptAPI::EventID EVENT_PAIR_A = 47312;
	constexpr SkyPromptAPI::EventID EVENT_PAIR_B = 47316;
	constexpr SkyPromptAPI::EventID EVENT_DESTROY = 47315;

	constexpr SkyPromptAPI::ActionID ACTION_PAIR_A = 0;
	constexpr SkyPromptAPI::ActionID ACTION_PAIR_B = 1;
	constexpr SkyPromptAPI::ActionID ACTION_DESTROY = 3;

	SkyPromptAPI::ClientID    g_clientID = 0;
	std::atomic<bool>         g_showing{ false };
	std::atomic<RE::FormID>   g_currentRefID{ 0 };

	// Grave (Destroy Grave) prompt state — separate sink, revealed by a
	// held modifier so it never clutters the epitaph.
	std::atomic<RE::FormID>   g_currentGraveID{ 0 };
	std::atomic<bool>         g_graveShowing{ false };

	// Reveal modifier (Shift) currently held.
	std::atomic<bool>         g_modifierHeld{ false };

	// One key-pair (keyboard, gamepad) per PROMPT. Pair A = Lay/Bury key,
	// Pair B = Resurrect/Take key.
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_pairAKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_pairBKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_graveKeys[2];

	// Tap-vs-hold state per pair: kAccepted (hold bar filled) sets it; kUp
	// reads-and-clears it — a release with the flag still false was a TAP.
	std::atomic<bool> g_holdFiredA{ false };
	std::atomic<bool> g_holdFiredB{ false };

	struct RespectPromptSink : public SkyPromptAPI::PromptSink
	{
		// Two paired kHold templates. A = Lay(tap)/Bury(hold) on the Lay key,
		// B = Resurrect(tap)/Take(hold) on the Resurrect key. B drops out on an
		// ash pile (nothing to resurrect); rebuilt each show.
		SkyPromptAPI::Prompt m_templates[2];
		SkyPromptAPI::Prompt m_visible[2];
		std::atomic<std::uint8_t> m_promptCount{ 1 };

		RespectPromptSink()
		{
			// Pair A — tap: Lay to Rest, hold: Bury with Gravestone.
			m_templates[0].text = "Lay to Rest  (hold: Bury)";
			m_templates[0].eventID = EVENT_PAIR_A;
			m_templates[0].actionID = ACTION_PAIR_A;
			m_templates[0].type = SkyPromptAPI::PromptType::kHold;
			m_templates[0].refid = 0;
			m_templates[0].text_color = 0xFFFFFFFF;
			m_templates[0].progress = 0.0f;

			// Pair B — tap: Resurrect, hold: Take Body.
			m_templates[1].text = "Resurrect  (hold: Take Body)";
			m_templates[1].eventID = EVENT_PAIR_B;
			m_templates[1].actionID = ACTION_PAIR_B;
			m_templates[1].type = SkyPromptAPI::PromptType::kHold;
			m_templates[1].refid = 0;
			m_templates[1].text_color = 0xFFFFFFFF;
			m_templates[1].progress = 0.0f;
		}

		// Pair A always shows (Lay works on any corpse); Pair B shows only when
		// the target can be resurrected (a dead actor, not an ash pile).
		void BuildVisible(RE::FormID a_refID, bool a_wantB)
		{
			std::uint8_t n = 0;
			m_visible[n] = m_templates[0];  // Pair A (Lay/Bury) always
			m_visible[n].refid = a_refID;
			++n;
			if (a_wantB) {
				m_visible[n] = m_templates[1];  // Pair B (Resurrect/Take)
				m_visible[n].refid = a_refID;
				++n;
			}
			m_promptCount.store(n);
		}

		std::span<const SkyPromptAPI::Prompt> GetPrompts() const override
		{
			return std::span<const SkyPromptAPI::Prompt>(m_visible, m_promptCount.load());
		}

		// Tap = a quick press+release (kUp arrives with no preceding kAccepted);
		// hold = the bar fills (kAccepted). kDown resets the per-pair hold flag.
		void ProcessEvent(SkyPromptAPI::PromptEvent a_event) const override
		{
			const RE::FormID refID = g_currentRefID.load();
			if (refID == 0) return;

			const auto action = a_event.prompt.actionID;
			const auto etype  = a_event.type;
			auto* task = SKSE::GetTaskInterface();
			if (!task) return;

			using ET = SkyPromptAPI::PromptEventType;

			if (action == ACTION_PAIR_A) {
				if (etype == ET::kDown) {
					g_holdFiredA.store(false);
				} else if (etype == ET::kAccepted) {          // hold → Bury
					g_holdFiredA.store(true);
					task->AddTask([refID]() { RespectManager::ExecuteBury(refID); });
				} else if (etype == ET::kUp) {                // release
					if (!g_holdFiredA.exchange(false)) {      // no hold fired → tap → Lay
						task->AddTask([refID]() { RespectManager::ExecuteLayToRest(refID); });
					}
				}
			} else if (action == ACTION_PAIR_B) {
				if (etype == ET::kDown) {
					g_holdFiredB.store(false);
				} else if (etype == ET::kAccepted) {          // hold → Take Body
					g_holdFiredB.store(true);
					task->AddTask([refID]() { PickupManager::ExecuteCollect(refID); });
				} else if (etype == ET::kUp) {                // release
					if (!g_holdFiredB.exchange(false)) {      // no hold fired → tap → Resurrect
						task->AddTask([refID]() { RespectManager::ExecuteResurrect(refID); });
					}
				}
			}
		}
	};

	RespectPromptSink g_sink;

	// Send / hide the body prompt set.
	void SendBodyPrompt()
	{
		if (g_clientID == 0) return;
		if (!g_showing.load()) {
			(void)SkyPromptAPI::SendPrompt(&g_sink, g_clientID);
			g_showing.store(true);
		}
	}

	void HideBodyPrompt()
	{
		if (g_showing.load()) {
			SkyPromptAPI::RemovePrompt(&g_sink, g_clientID);
			g_showing.store(false);
		}
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
	// corpse, or the Destroy Grave prompt on one of our graves.  Releasing hides
	// them again.
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
						SendBodyPrompt();
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
		// Pair A (Lay tap / Bury hold) uses the Lay key; Pair B (Resurrect tap /
		// Take hold) uses the Resurrect key. The old separate bury/collect keys
		// are unused now that each pair shares one key via tap-vs-hold.
		g_pairAKeys[0] = { RE::INPUT_DEVICE::kKeyboard,
			static_cast<SkyPromptAPI::ButtonID>(s.layKeyboard.load()) };
		g_pairAKeys[1] = { RE::INPUT_DEVICE::kGamepad,
			static_cast<SkyPromptAPI::ButtonID>(s.layGamepad.load()) };
		g_pairBKeys[0] = { RE::INPUT_DEVICE::kKeyboard,
			static_cast<SkyPromptAPI::ButtonID>(s.resurrectKeyboard.load()) };
		g_pairBKeys[1] = { RE::INPUT_DEVICE::kGamepad,
			static_cast<SkyPromptAPI::ButtonID>(s.resurrectGamepad.load()) };

		g_sink.m_templates[0].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_pairAKeys, 2);
		g_sink.m_templates[1].button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_pairBKeys, 2);

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

	(void)SkyPromptAPI::RequestTheme(g_clientID, "BuryTakeBodies");
	logger::info("PromptManager: init clientID={} (PairA/Lay-Bury=kb{} PairB/Res-Take=kb{}) shiftGated={}",
		g_clientID,
		PFR::Settings::GetSingleton().layKeyboard.load(),
		PFR::Settings::GetSingleton().resurrectKeyboard.load(),
		PFR::Settings::GetSingleton().shiftGatesPrompts.load());
}

void PromptManager::Shutdown()
{
	HideBodyPrompt();
	g_currentRefID.store(0);
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

	// If the crosshair moved to a different corpse, drop the old prompt set so
	// we can rebuild it (Bury/PickUp availability may differ).
	if (g_currentRefID.load() != refID) {
		HideBodyPrompt();
	}
	g_currentRefID.store(refID);

	auto& s = PFR::Settings::GetSingleton();

	// Pair A (Lay tap / Bury hold) always shows — Lay works on any corpse, and
	// the shovel gate for Bury is enforced at execute time. Pair B (Resurrect
	// tap / Take hold) shows only on a resurrectable dead actor (not an ash pile).
	const bool wantB = RespectManager::CanResurrect(a_ref);
	g_sink.BuildVisible(refID, wantB);

	if (!s.shiftGatesPrompts.load()) {
		// Always-on behaviour: show the prompts whenever the crosshair is on a corpse.
		SendBodyPrompt();
	} else if (g_modifierHeld.load()) {
		// Modifier already held as we look at the corpse — reveal immediately.
		SendBodyPrompt();
	}
	// Otherwise the prompts stay hidden until the reveal modifier is pressed
	// (ModifierInputSink) — so a corpse shows only its default (e.g. QuickLoot)
	// until you hold Shift.
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
