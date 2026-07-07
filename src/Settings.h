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

		// Destroy-grave: hold this MODIFIER while looking at one of our graves
		// to reveal a "Destroy Grave" hold prompt on graveDestroyKey.  Hidden
		// otherwise so the epitaph stays clean.  LShift = 0x2A (42), E = 0x12 (18).
		std::atomic<bool>    graveDestroyEnabled{ true };
		std::atomic<int32_t> graveDestroyModifier{ 42 };
		std::atomic<int32_t> graveDestroyKey{ 18 };

		// When true the grave engraves the runtime display name
		// (Real Names / any rename mod).  When false, the base-object name
		// ("Bandit") like stock Pay Your Respects.
		std::atomic<bool> useDisplayName{ true };

		// Bury requires a vanilla shovel (Shovel01 0xF5D05 / Shovel02 0xF5D06)
		// in the player's inventory.  The Bury prompt is hidden without one;
		// Lay to Rest and Pick Up Body are unaffected.
		std::atomic<bool> buryRequiresShovel{ true };

		// Gate the corpse action prompts behind the reveal modifier (Shift):
		// with this ON, a corpse shows QuickLoot by default and our
		// Lay/Bury/PickUp prompts appear only WHILE the modifier is held (which
		// also hides QuickLoot).  OFF = old behaviour (prompts always shown,
		// QuickLoot left alone).  Reuses graveDestroyModifier as the key.
		std::atomic<bool> shiftGatesPrompts{ true };

	private:
		Settings() = default;
	};
}
