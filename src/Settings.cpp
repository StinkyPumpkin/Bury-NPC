#include "pch.h"
#include "Settings.h"

namespace PFR
{
	Settings& Settings::GetSingleton()
	{
		static Settings instance;
		return instance;
	}

	void Settings::Load()
	{
		const wchar_t* path = L"Data/SKSE/Plugins/PressFtoPayRespects.ini";

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

		useDisplayName.store(
			ini.GetBoolValue("Grave", "useDisplayName", useDisplayName.load()));

		logger::info(
			"Settings: layKB={} buryKB={} collectKB={} collectEnabled={} "
			"useDisplayName={} debug={}",
			layKeyboard.load(), buryKeyboard.load(), collectKeyboard.load(),
			collectEnabled.load(), useDisplayName.load(), debug.load());
	}
}
