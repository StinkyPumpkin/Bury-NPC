#include "pch.h"
#include "Settings.h"

#include <fstream>

namespace PFR
{
	Settings& Settings::GetSingleton()
	{
		static Settings instance;
		return instance;
	}

	namespace
	{
		// 2026-09-26: the ini is no longer shipped (a redeploy would reset the user's values).
		// The defaults live in Settings.h; this commented copy of them is written once, on the
		// first run that finds the file missing (under MO2 it lands in overwrite).
		constexpr const char* kDefaultIni = R"INI(; ============================================================================
; Quick presets:
;   LITE  (no shovel, instant):  requiresShovel=false  buryAnimation=false
;   FULL  (shovel gate + dig):   requiresShovel=true   buryAnimation=true
; shiftGatesPrompts is independent of the above (see below).
; ============================================================================

[General]
; enable verbose logging to BuryTakeBodies.log
debug = false
; shiftGatesPrompts = true: the two tap/hold prompts appear only WHILE you hold
; the reveal modifier (graveDestroyModifier, LShift by default) - so a corpse
; stays clean (showing just its default, e.g. a loot-menu mod) until you hold
; Shift. false = prompts always shown on a corpse.
shiftGatesPrompts = true
; while Shift is held on a corpse, QuickLoot IE's loot list is hidden (3.x request API)
; so the E tap (Resurrect) / hold (Take) never also picks a loot item. Released = QuickLoot comes back.
hideQuickLootWhileRevealing = true

[Bury]
; requiresShovel = true: burying (the HOLD on the layToRest key) needs a vanilla
; shovel (Shovel01 0xF5D05 / Shovel02 0xF5D06) - hold without one and you get a
; "need a shovel" notice. Lay to Rest (the TAP) is unaffected. false = no shovel needed.
requiresShovel = true
; buryAnimation = true: play the vanilla smelter-shovel "dig" animation when you
; Bury (menu-less isSmelter furniture). false = grave appears instantly.
buryAnimation = true
; seconds the dig animation plays before the grave appears + body is removed.
buryAnimationDelay = 4.0

[Hotkeys]
; TWO keys, each a TAP/HOLD prompt (revealed while you hold Shift on a corpse):
;   layToRest key  - TAP = Lay to Rest,  HOLD = Bury with Gravestone
;   resurrect key  - TAP = Resurrect,    HOLD = Take Body
; DX scancodes: F = 33 (0x21), E = 18 (0x12). Gamepad -1 = disabled.
layToRestKeyboard = 33
layToRestGamepad = -1
resurrectKeyboard = 18
resurrectGamepad = -1
; buryKeyboard / collectKeyboard are NO LONGER USED - Bury is the HOLD of the
; layToRest key, and Take Body is the HOLD of the resurrect key.
buryKeyboard = 19
buryGamepad = -1
collectKeyboard = 20
collectGamepad = -1

[Collect]
; NOTE: currently ignored - Take Body is the HOLD of the resurrect key, so it
; shares that key's prompt (can't be hidden on its own under the 2-key layout).
collectEnabled = true

[Resurrect]
; NOTE: currently ignored - Resurrect + Take share the resurrect key's prompt,
; which shows on any dead actor. (Left here for a future per-action toggle.)
resurrectEnabled = true
; true  = the actor comes back fully re-equipped (like console "resurrect 1").
; false = they keep their current (possibly looted) inventory.
resurrectResetInventory = true

[Grave]
; useDisplayName = true engraves the runtime name (Real Names / rename mods).
; false = base-object name ("Bandit") like stock Pay Your Respects.
useDisplayName = true

; Destroy a grave you placed: look at it, HOLD the modifier (reveal), then HOLD
; the key to dig it out. Hidden until you hold the modifier so the epitaph
; stays clean. DX scancodes: LShift = 42, E = 18. Only affects this mod's graves.
graveDestroyEnabled = true
graveDestroyModifier = 42
graveDestroyKey = 18
)INI";

		void WriteDefaultIniIfMissing(const std::filesystem::path& a_path)
		{
			std::error_code ec;
			if (std::filesystem::exists(a_path, ec)) {
				return;
			}
			std::filesystem::create_directories(a_path.parent_path(), ec);
			std::ofstream out(a_path, std::ios::out | std::ios::trunc);
			if (!out) {
				logger::warn("Settings: could not create {} - using built-in defaults", a_path.string());
				return;
			}
			out << kDefaultIni;
			logger::info("Settings: {} was missing - wrote the defaults", a_path.string());
		}
	}

	void Settings::Load()
	{
		const wchar_t* path = L"Data/SKSE/Plugins/BuryTakeBodies.ini";

		WriteDefaultIniIfMissing(std::filesystem::path(path));

		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(path) < 0) {
			logger::info("Settings: no ini found, using defaults");
			return;
		}

		debug.store(ini.GetBoolValue("General", "debug", debug.load()));

		layKeyboard.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "layToRestKeyboard", layKeyboard.load())));
		layGamepad.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "layToRestGamepad", layGamepad.load())));
		buryKeyboard.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "buryKeyboard", buryKeyboard.load())));
		buryGamepad.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "buryGamepad", buryGamepad.load())));
		collectKeyboard.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "collectKeyboard", collectKeyboard.load())));
		collectGamepad.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "collectGamepad", collectGamepad.load())));

		collectEnabled.store(
			ini.GetBoolValue("Collect", "collectEnabled", collectEnabled.load()));

		resurrectKeyboard.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "resurrectKeyboard", resurrectKeyboard.load())));
		resurrectGamepad.store(static_cast<int32_t>(
			ini.GetLongValue("Hotkeys", "resurrectGamepad", resurrectGamepad.load())));
		resurrectEnabled.store(
			ini.GetBoolValue("Resurrect", "resurrectEnabled", resurrectEnabled.load()));
		resurrectResetInventory.store(
			ini.GetBoolValue("Resurrect", "resurrectResetInventory", resurrectResetInventory.load()));

		graveDestroyEnabled.store(
			ini.GetBoolValue("Grave", "graveDestroyEnabled", graveDestroyEnabled.load()));
		graveDestroyModifier.store(static_cast<int32_t>(
			ini.GetLongValue("Grave", "graveDestroyModifier", graveDestroyModifier.load())));
		graveDestroyKey.store(static_cast<int32_t>(
			ini.GetLongValue("Grave", "graveDestroyKey", graveDestroyKey.load())));

		useDisplayName.store(
			ini.GetBoolValue("Grave", "useDisplayName", useDisplayName.load()));

		buryRequiresShovel.store(
			ini.GetBoolValue("Bury", "requiresShovel", buryRequiresShovel.load()));
		buryAnimation.store(
			ini.GetBoolValue("Bury", "buryAnimation", buryAnimation.load()));
		buryAnimationDelay.store(static_cast<float>(
			ini.GetDoubleValue("Bury", "buryAnimationDelay", buryAnimationDelay.load())));
		shiftGatesPrompts.store(
			ini.GetBoolValue("General", "shiftGatesPrompts", shiftGatesPrompts.load()));
		hideQuickLootWhileRevealing.store(
			ini.GetBoolValue("General", "hideQuickLootWhileRevealing", hideQuickLootWhileRevealing.load()));

		logger::info(
			"Settings: layKB={} buryKB={} collectKB={} collectEnabled={} "
			"useDisplayName={} debug={}",
			layKeyboard.load(), buryKeyboard.load(), collectKeyboard.load(),
			collectEnabled.load(), useDisplayName.load(), debug.load());
	}
}
