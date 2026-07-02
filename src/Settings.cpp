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

		useDisplayName.store(
			ini.GetBoolValue("Grave", "useDisplayName", useDisplayName.load()));
		includePlayerName.store(
			ini.GetBoolValue("Grave", "includePlayerName", includePlayerName.load()));

		logger::info(
			"Settings: layKB={} buryKB={} useDisplayName={} includePlayerName={} debug={}",
			layKeyboard.load(), buryKeyboard.load(),
			useDisplayName.load(), includePlayerName.load(), debug.load());
	}
}
