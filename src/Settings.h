#pragma once

namespace PFR
{
	// Simple ini-backed config.  No MCM, no perk gate (removed from the
	// original Press F when forking).  Two hold actions on two keys.
	struct Settings
	{
		static Settings& GetSingleton();

		void Load();

		std::atomic<bool> debug{ false };

		// DX scancodes.  F = 0x21 (33), R = 0x13 (19) by default.
		std::atomic<int32_t> layKeyboard{ 33 };
		std::atomic<int32_t> layGamepad{ -1 };
		std::atomic<int32_t> buryKeyboard{ 19 };
		std::atomic<int32_t> buryGamepad{ -1 };

		// Pick-up-body hold action.  T = 0x14 (20).
		std::atomic<int32_t> collectKeyboard{ 20 };
		std::atomic<int32_t> collectGamepad{ -1 };
		std::atomic<bool>    collectEnabled{ true };

		// When true the grave engraves the runtime display name
		// (Real Names / any rename mod).  When false, the base-object name
		// ("Bandit") like stock Pay Your Respects.
		std::atomic<bool> useDisplayName{ true };

		// Prepend "Buried by <player>" line to the memorial.
		std::atomic<bool> includePlayerName{ true };

	private:
		Settings() = default;
	};
}
