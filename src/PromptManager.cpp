#include "pch.h"
#include "PromptManager.h"
#include "Settings.h"
#include "RespectManager.h"
#include "PickupManager.h"
#include "QuickLootAPI.h"      // QuickLoot IE 3.x request API (SKSE messaging), header-only
#include "QuickLootAPI_v4.h"   // QuickLoot IE 4.x exported-interface API (MIT), header-only

#include <Xinput.h>             // XINPUT_STATE only; XInputGetState is looked up at runtime

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
	constexpr SkyPromptAPI::EventID EVENT_EPITAPH = 47317;

	constexpr SkyPromptAPI::ActionID ACTION_PAIR_A = 0;
	constexpr SkyPromptAPI::ActionID ACTION_PAIR_B = 1;
	constexpr SkyPromptAPI::ActionID ACTION_DESTROY = 3;
	constexpr SkyPromptAPI::ActionID ACTION_EPITAPH = 4;

	SkyPromptAPI::ClientID    g_clientID = 0;
	std::atomic<bool>         g_showing{ false };
	std::atomic<RE::FormID>   g_currentRefID{ 0 };

	// Grave (Destroy Grave) prompt state — separate sink, revealed by a
	// held modifier so it never clutters the epitaph.
	std::atomic<RE::FormID>   g_currentGraveID{ 0 };
	std::atomic<bool>         g_graveShowing{ false };

	// Reveal modifier (Shift) currently held.
	std::atomic<bool>         g_modifierHeld{ false };

	// --Claude 2026-09-07: QuickLoot IE suppression while the reveal modifier is held on
	// a corpse. DisableLootMenu hides the loot list at once; EnableLootMenu makes
	// QuickLoot re-evaluate the crosshair and re-open on the same corpse.
	std::atomic<bool>         g_quickLootReady{ false };
	std::atomic<bool>         g_quickLootV4{ false };   // 4.x dropped the 3.x messaging API
	std::atomic<bool>         g_lootSuppressed{ false };

	void QuickLootDisable()
	{
		if (g_quickLootV4.load()) QuickLoot::API::QuickLootAPI::DisableLootMenu();
		else QuickLoot::QuickLootAPI::DisableLootMenu();
	}

	void QuickLootEnable()
	{
		if (g_quickLootV4.load()) QuickLoot::API::QuickLootAPI::EnableLootMenu();
		else QuickLoot::QuickLootAPI::EnableLootMenu();
	}

	void SuppressQuickLoot(bool a_on)
	{
		if (!g_quickLootReady.load()) return;
		if (!PFR::Settings::GetSingleton().hideQuickLootWhileRevealing.load()) return;
		if (a_on) {
			if (!g_lootSuppressed.exchange(true)) QuickLootDisable();
		} else {
			if (g_lootSuppressed.exchange(false)) QuickLootEnable();
		}
	}

	// One key-pair (keyboard, gamepad) per PROMPT. Pair A = Lay/Bury key,
	// Pair B = Resurrect/Take key.
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_pairAKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_pairBKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_graveKeys[2];
	std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID> g_epitaphKeys[2];   // the Lay key

	// Tap-vs-hold state per pair: kAccepted (hold bar filled) sets it; kUp
	// reads-and-clears it — a release with the flag still false was a TAP.
	std::atomic<bool> g_holdFiredA{ false };
	std::atomic<bool> g_holdFiredB{ false };

	// --Claude 2026-10-12: g_modifierHeld can go stale when the reveal key's release never
	// reaches our input sink (alt-tab, a menu, a scene), which left the prompts live so a
	// plain E resurrected. A pair is ARMED on its kDown only while the reveal key is
	// physically down; kAccepted / kUp act only on an armed press.
	std::atomic<bool> g_armedA{ false };
	std::atomic<bool> g_armedB{ false };

	// The body a press started on. Turning to another corpse while the key is
	// still down re-sends the prompts there, and the release would then have
	// tapped (Lay to Rest / Resurrect) the NEW body. An action fires only on the
	// body its press began on.
	std::atomic<RE::FormID> g_pressRefA{ 0 };
	std::atomic<RE::FormID> g_pressRefB{ 0 };

	// True when this release/fill belongs to the press that started on a_refID;
	// otherwise the press is dropped.
	bool SamePress(std::atomic<bool>& a_armed, std::atomic<RE::FormID>& a_pressRef, RE::FormID a_refID)
	{
		if (a_refID != 0 && a_pressRef.load() == a_refID) return true;
		a_armed.store(false);
		return false;
	}

	void HideBodyPrompt();

	// The DirectInput state array read directly (calling BSWin32KeyboardDevice::IsPressed
	// links CommonLib's device object, whose virtuals it never defines - see TETHER Gate.cpp).
	bool KeyboardRevealDown()
	{
		const auto key = static_cast<std::uint32_t>(PFR::Settings::GetSingleton().graveDestroyModifier.load());
		if (key == 0 || key >= 256) return false;
		auto* mgr = RE::BSInputDeviceManager::GetSingleton();
		auto* kb  = mgr ? mgr->GetKeyboard() : nullptr;
		if (!kb) return false;
		const auto& keys = kb->GetRuntimeData().curState;
		return key < sizeof(keys) && (keys[key] & 0x80) != 0;
	}

	// ---- Controller reveal (revealGamepad, SKSE gamepad keycode 266-281) ----
	// The engine's gamepad ButtonEvent carries the XInput button mask; triggers
	// come through as 0x9 (LT) / 0xA (RT).
	constexpr std::uint32_t kPadMasks[16] = {
		0x0001, 0x0002, 0x0004, 0x0008,   // 266-269 D-pad up/down/left/right
		0x0010, 0x0020, 0x0040, 0x0080,   // 270-273 Start, Back, LS, RS
		0x0100, 0x0200, 0x1000, 0x2000,   // 274-277 LB, RB, A, B
		0x4000, 0x8000, 0x0009, 0x000A    // 278-281 X, Y, LT, RT
	};

	std::uint32_t PadKeycodeToMask(std::int32_t a_keycode)
	{
		if (a_keycode < 266 || a_keycode > 281) return 0;
		return kPadMasks[a_keycode - 266];
	}

	// The real button state straight from XInput (controller 0), so a lost
	// release can't leave the reveal stuck - the same reason the keyboard side
	// reads curState. Loaded at runtime: no import-library dependency.
	using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
	XInputGetState_t GetXInputGetState()
	{
		static XInputGetState_t fn = []() -> XInputGetState_t {
			for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" }) {
				HMODULE mod = ::GetModuleHandleW(dll);
				if (!mod) mod = ::LoadLibraryW(dll);
				if (mod) {
					if (auto p = reinterpret_cast<XInputGetState_t>(::GetProcAddress(mod, "XInputGetState"))) return p;
				}
			}
			return nullptr;
		}();
		return fn;
	}

	std::atomic<bool> g_padRevealSeen{ false };   // fallback when XInput can't be read

	bool PadRevealDown()
	{
		const std::int32_t code = PFR::Settings::GetSingleton().revealGamepad.load();
		const std::uint32_t mask = PadKeycodeToMask(code);
		if (mask == 0) return false;
		auto getState = GetXInputGetState();
		if (!getState) return g_padRevealSeen.load();
		XINPUT_STATE st{};
		if (getState(0, &st) != ERROR_SUCCESS) return false;
		if (code == 280) return st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
		if (code == 281) return st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
		return (st.Gamepad.wButtons & mask) != 0;
	}

	// Shift on the keyboard OR the reveal button on the controller.
	bool ModifierPhysicallyDown()
	{
		return KeyboardRevealDown() || PadRevealDown();
	}

	// A press is allowed when Shift-gating is off, or the reveal key is really down now.
	bool RevealSatisfied()
	{
		return !PFR::Settings::GetSingleton().shiftGatesPrompts.load() || ModifierPhysicallyDown();
	}

	// The prompts were live without the reveal key: drop the stale state on the main thread.
	void ClearStaleReveal()
	{
		if (auto* task = SKSE::GetTaskInterface()) {
			task->AddTask([]() {
				g_modifierHeld.store(false);
				HideBodyPrompt();
				SuppressQuickLoot(false);
			});
		}
	}

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
		// the target can be resurrected (a dead actor, not an ash pile) and at
		// least one of its halves is enabled. --Claude 2026-10-04: the label drops
		// a half switched off by [Resurrect] resurrectEnabled / [Collect] collectEnabled.
		// a_urn: a dug-up burial urn - only the holds do something there (Bury it
		// again, Pick it up); the taps (Lay to Rest, Resurrect) do nothing.
		// a_ashes: an ash pile - its hold gathers the ashes into an urn and picks that up.
		void BuildVisible(RE::FormID a_refID, bool a_wantA, bool a_wantB, bool a_resurrect, bool a_collect, bool a_urn = false,
			bool a_ashes = false)
		{
			std::uint8_t n = 0;
			if (a_wantA) {
				m_visible[n] = m_templates[0];  // Pair A (Lay/Bury)
				m_visible[n].refid = a_refID;
				if (a_urn) m_visible[n].text = "Bury (hold)";
				++n;
			}
			if (a_wantB && (a_resurrect || a_collect)) {
				m_visible[n] = m_templates[1];  // Pair B (Resurrect/Take)
				m_visible[n].refid = a_refID;
				if (a_urn || a_ashes) m_visible[n].text = "Pick Up (hold)";
				else if (!a_resurrect) m_visible[n].text = "Take Body (hold)";
				else if (!a_collect) m_visible[n].text = "Resurrect";
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

			const auto action = a_event.prompt.actionID;
			const auto etype  = a_event.type;
			auto* task = SKSE::GetTaskInterface();
			if (!task) return;

			using ET = SkyPromptAPI::PromptEventType;

			if (action == ACTION_PAIR_A) {
				if (etype == ET::kDown) {
					g_holdFiredA.store(false);
					g_pressRefA.store(refID);
					g_armedA.store(refID != 0 && RevealSatisfied());
					if (refID != 0 && !g_armedA.load()) ClearStaleReveal();
				} else if (etype == ET::kAccepted) {          // hold → Bury
					if (!SamePress(g_armedA, g_pressRefA, refID)) return;
					if (!g_armedA.load()) return;
					g_holdFiredA.store(true);
					task->AddTask([refID]() { RespectManager::ExecuteBury(refID); });
				} else if (etype == ET::kUp) {                // release
					if (!SamePress(g_armedA, g_pressRefA, refID)) return;
					if (!g_armedA.exchange(false)) return;
					if (!g_holdFiredA.exchange(false)) {      // no hold fired → tap → Lay
						task->AddTask([refID]() { RespectManager::ExecuteLayToRest(refID); });
					}
				}
			} else if (action == ACTION_PAIR_B) {
				auto& s = PFR::Settings::GetSingleton();
				if (etype == ET::kDown) {
					g_holdFiredB.store(false);
					g_pressRefB.store(refID);
					g_armedB.store(refID != 0 && RevealSatisfied());
					if (refID != 0 && !g_armedB.load()) ClearStaleReveal();
				} else if (etype == ET::kAccepted) {          // hold → Take Body
					if (!SamePress(g_armedB, g_pressRefB, refID)) return;
					if (!g_armedB.load()) return;
					g_holdFiredB.store(true);
					if (s.collectEnabled.load()) {
						task->AddTask([refID]() {
							if (!RespectManager::PickUpAshPile(refID)) PickupManager::ExecuteCollect(refID);
						});
					}
				} else if (etype == ET::kUp) {                // release
					if (!SamePress(g_armedB, g_pressRefB, refID)) return;
					if (!g_armedB.exchange(false)) return;
					if (!g_holdFiredB.exchange(false)) {      // no hold fired → tap → Resurrect
						if (s.resurrectEnabled.load()) {
							task->AddTask([refID]() { RespectManager::ExecuteResurrect(refID); });
						}
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
		if (g_sink.m_promptCount.load() == 0) return;   // both keys set to -1
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
	// Grave prompts, shown only while the modifier is held on one of our
	// graves: Edit Epitaph (a press of the Lay key) and Destroy Grave / Dig Up
	// (a hold of the grave key).
	// ------------------------------------------------------------------
	struct GravePromptSink : public SkyPromptAPI::PromptSink
	{
		SkyPromptAPI::Prompt m_epitaph;
		SkyPromptAPI::Prompt m_prompt;    // Destroy Grave / Dig Up
		SkyPromptAPI::Prompt m_visible[2];
		std::atomic<std::uint8_t> m_count{ 0 };

		GravePromptSink()
		{
			m_epitaph.text = "Edit Epitaph";
			m_epitaph.eventID = EVENT_EPITAPH;
			m_epitaph.actionID = ACTION_EPITAPH;
			m_epitaph.type = SkyPromptAPI::PromptType::kSinglePress;
			m_epitaph.refid = 0;
			m_epitaph.text_color = 0xFFFFFFFF;
			m_epitaph.progress = 0.0f;

			m_prompt.text = "Destroy Grave";
			m_prompt.eventID = EVENT_DESTROY;
			m_prompt.actionID = ACTION_DESTROY;
			m_prompt.type = SkyPromptAPI::PromptType::kHold;
			m_prompt.refid = 0;
			m_prompt.text_color = 0xFFFFFFFF;
			m_prompt.progress = 0.0f;
		}

		void BuildVisible(bool a_epitaph, bool a_destroy)
		{
			std::uint8_t n = 0;
			if (a_epitaph) m_visible[n++] = m_epitaph;
			if (a_destroy) m_visible[n++] = m_prompt;
			m_count.store(n);
		}

		std::span<const SkyPromptAPI::Prompt> GetPrompts() const override
		{
			return std::span<const SkyPromptAPI::Prompt>(m_visible, m_count.load());
		}

		void ProcessEvent(SkyPromptAPI::PromptEvent a_event) const override
		{
			if (a_event.type != SkyPromptAPI::PromptEventType::kAccepted) return;
			RE::FormID graveID = g_currentGraveID.load();
			if (graveID == 0) return;
			if (!ModifierPhysicallyDown()) return;   // --Claude 2026-10-12: stale reveal, see g_armedA
			if (a_event.prompt.actionID == ACTION_EPITAPH) {
				SKSE::GetTaskInterface()->AddTask([graveID]() {
					RespectManager::ExecuteEditEpitaph(graveID);
				});
			} else if (a_event.prompt.actionID == ACTION_DESTROY) {
				SKSE::GetTaskInterface()->AddTask([graveID]() {
					RespectManager::DestroyGrave(graveID);
				});
			}
		}
	};

	GravePromptSink g_graveSink;

	void SendGravePrompt()
	{
		if (g_clientID == 0) return;
		RE::FormID graveID = g_currentGraveID.load();
		if (graveID == 0) return;
		auto& s = PFR::Settings::GetSingleton();
		// graveDestroyEnabled = false hides Destroy Grave, but a grave holding a
		// person's remains can still be dug up - otherwise their things are lost.
		const bool hasRemains = RespectManager::GraveHasRemains(graveID);
		const bool destroy = s.graveDestroyEnabled.load() || hasRemains;
		// Edit Epitaph is on the Lay key; both its keys at -1 switch it off.
		const bool epitaph = s.layKeyboard.load() >= 0 || s.layGamepad.load() >= 0;
		// refid 0 = not anchored to the grave's on-screen position (a grave sits
		// on the ground, which would drag the prompt to the bottom of the
		// screen / off the left edge).  With 0 it renders at the theme's fixed
		// centre.  We track the target grave in g_currentGraveID, not refid.
		g_graveSink.m_prompt.refid = 0;
		// A grave holding a person's remains is dug up instead of destroyed.
		g_graveSink.m_prompt.text = hasRemains ? "Dig Up" : "Destroy Grave";
		g_graveSink.BuildVisible(epitaph, destroy);
		if (g_graveSink.m_count.load() == 0) return;
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

			bool sawModifier = false;
			const std::uint32_t padMask = PadKeycodeToMask(s.revealGamepad.load());
			for (auto* e = *a_events; e; e = e->next) {
				if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
				auto* btn = e->AsButtonEvent();
				if (!btn) continue;
				const auto device = btn->GetDevice();
				if (device == RE::INPUT_DEVICE::kKeyboard) {
					if (btn->GetIDCode() != modKey) continue;
				} else if (device == RE::INPUT_DEVICE::kGamepad) {
					if (padMask == 0 || btn->GetIDCode() != padMask) continue;
					g_padRevealSeen.store(btn->IsPressed());
				} else {
					continue;
				}
				sawModifier = true;

				if (btn->IsDown()) {
					g_modifierHeld.store(true);
					if (g_currentRefID.load() != 0 && s.shiftGatesPrompts.load()) {
						SuppressQuickLoot(true);   // loot list away first, then our buttons
						SendBodyPrompt();
					} else if (g_currentGraveID.load() != 0) {
						SendGravePrompt();
					}
				} else if (btn->IsUp() && !ModifierPhysicallyDown()) {   // the other reveal may still be held
					g_modifierHeld.store(false);
					if (s.shiftGatesPrompts.load()) HideBodyPrompt();
					RemoveGravePrompt();
					SuppressQuickLoot(false);
				}
			}

			// The Shift release can be lost (a one-frame tap reports no held time, so
			// IsUp() is false; alt-tab or a menu can swallow it). Any later input -
			// a mouse move is enough - re-reads the real key and drops prompts that
			// are still up without it.
			if (!sawModifier && (g_modifierHeld.load() || g_graveShowing.load() ||
				(g_showing.load() && s.shiftGatesPrompts.load())) && !ModifierPhysicallyDown()) {
				g_modifierHeld.store(false);
				if (s.shiftGatesPrompts.load()) HideBodyPrompt();
				RemoveGravePrompt();
				SuppressQuickLoot(false);
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
			static_cast<SkyPromptAPI::ButtonID>(s.graveDestroyGamepad.load()) };
		g_graveSink.m_prompt.button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_graveKeys, 2);
		// Edit Epitaph is a press of the Lay key (F) on a grave.
		g_epitaphKeys[0] = g_pairAKeys[0];
		g_epitaphKeys[1] = g_pairAKeys[1];
		g_graveSink.m_epitaph.button_key =
			std::span<const std::pair<RE::INPUT_DEVICE, SkyPromptAPI::ButtonID>>(g_epitaphKeys, 2);
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
			// A corpse with activation blocked belongs to someone else right now:
			// RSE - Shoulder Or Saddle blocks the body you carry on your shoulder,
			// quest scenes block theirs, and Bury blocks its own while the epitaph
			// box is up. No prompts on those, so nothing pulls a body out from
			// under another mod.
			if (ref && ref->IsActivationBlocked()) {
				PromptManager::Hide();
				return RE::BSEventNotifyControl::kContinue;
			}
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
				// --Claude 2026-09-18: ash piles only. This used to test the
				// base name of ANY ref, so a living NPC whose name contains
				// "ash" (Luxury Suite's Rashaal Taussa) got the corpse prompts
				// and Lay to Rest removed her. Actors never reach this branch
				// now; the name test only runs on non-actor refs.
				if (!actor && RespectManager::IsAshPile(ref.get())) {
					PromptManager::ShowForRef(ref.get());
					return RE::BSEventNotifyControl::kContinue;
				}
				// A dug-up burial urn: Bury it again or Pick it up, like a body.
				if (!actor && RespectManager::IsUrnRef(ref.get())) {
					PromptManager::ShowForRef(ref.get());
					return RE::BSEventNotifyControl::kContinue;
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

	// QuickLoot IE 3.x request API: connect now (kDataLoaded = every plugin is loaded).
	// Soft dependency — without QuickLoot IE the client just reports not-ready.
	// QuickLoot IE 4.x exports GetQuickLootInterfaceV20; 3.x only answers the messaging
	// request API. Try 4.x first so a QuickLoot update doesn't silently drop the hide.
	if (QuickLoot::API::QuickLootAPI::Init(Plugin::NAME, QuickLoot::API::ApiVersion::kV20)) {
		g_quickLootV4.store(true);
		g_quickLootReady.store(true);
	} else {
		QuickLoot::QuickLootAPI::Init();
		g_quickLootReady.store(QuickLoot::QuickLootAPI::IsReady());
	}
	logger::info("PromptManager: QuickLoot IE API {} (hideQuickLootWhileRevealing={})",
		g_quickLootReady.load() ? (g_quickLootV4.load() ? "connected (4.x interface)" : "connected (3.x requests)") : "not available",
		PFR::Settings::GetSingleton().hideQuickLootWhileRevealing.load());
	logger::info("PromptManager: init clientID={} (PairA/Lay-Bury=kb{} PairB/Res-Take=kb{}) shiftGated={}",
		g_clientID,
		PFR::Settings::GetSingleton().layKeyboard.load(),
		PFR::Settings::GetSingleton().resurrectKeyboard.load(),
		PFR::Settings::GetSingleton().shiftGatesPrompts.load());
	logger::info("PromptManager: controller reveal={} (Lay/Bury pad={} Res/Take pad={} grave pad={}) XInput={}",
		PFR::Settings::GetSingleton().revealGamepad.load(),
		PFR::Settings::GetSingleton().layGamepad.load(),
		PFR::Settings::GetSingleton().resurrectGamepad.load(),
		PFR::Settings::GetSingleton().graveDestroyGamepad.load(),
		GetXInputGetState() ? "yes" : "no");
}

void PromptManager::Shutdown()
{
	SuppressQuickLoot(false);
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

	// Pair A (Lay tap / Bury hold) shows unless its keys are -1 — Lay works on any corpse, and
	// the shovel gate for Bury is enforced at execute time. Pair B (Resurrect
	// tap / Take hold) shows only on a resurrectable dead actor (not an ash pile).
	// A pair whose keyboard AND gamepad keys are both -1 is switched off entirely
	// (it used to show with "???" as its key).
	const bool urn = RespectManager::IsUrnRef(a_ref);
	const bool ashes = !urn && RespectManager::IsAshPile(a_ref);
	const bool wantA = s.layKeyboard.load() >= 0 || s.layGamepad.load() >= 0;
	const bool wantB = (urn || ashes || RespectManager::CanResurrect(a_ref)) &&
		(s.resurrectKeyboard.load() >= 0 || s.resurrectGamepad.load() >= 0);
	g_sink.BuildVisible(refID, wantA, wantB,
		!urn && !ashes && s.resurrectEnabled.load() && !RespectManager::IsRaisedRemains(a_ref), s.collectEnabled.load(),
		urn, ashes);

	if (!s.shiftGatesPrompts.load()) {
		// Always-on behaviour: show the prompts whenever the crosshair is on a corpse.
		SendBodyPrompt();
	} else if (g_modifierHeld.load() && ModifierPhysicallyDown()) {
		// Modifier already held as we look at the corpse — reveal immediately.
		SuppressQuickLoot(true);
		SendBodyPrompt();
	} else if (g_modifierHeld.load()) {
		// --Claude 2026-10-12: the flag outlived the key (its release never reached us).
		g_modifierHeld.store(false);
	}
	// Otherwise the prompts stay hidden until the reveal modifier is pressed
	// (ModifierInputSink) — so a corpse shows only its default (e.g. QuickLoot)
	// until you hold Shift.
}

void PromptManager::ShowGraveForRef(RE::TESObjectREFR* a_ref)
{
	if (!a_ref || g_clientID == 0) return;

	// Grave and body prompts are mutually exclusive.
	HideBodyPrompt();
	g_currentRefID.store(0);

	// Just note which grave we're on — the grave prompts themselves only
	// appear when the reveal modifier is pressed (ModifierInputSink), so the
	// epitaph stays clean until the player deliberately holds it.
	g_currentGraveID.store(a_ref->GetFormID());
}

void PromptManager::Hide()
{
	SuppressQuickLoot(false);   // crosshair left the corpse — give QuickLoot back
	HideBodyPrompt();
	g_currentRefID.store(0);
	g_currentGraveID.store(0);
	RemoveGravePrompt();
}

void PromptManager::RefreshHotkeys()
{
	BuildButtonKeys();
}
