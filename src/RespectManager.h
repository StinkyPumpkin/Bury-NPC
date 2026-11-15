#pragma once

namespace RespectManager
{
	// Cache forms once the game data is loaded.
	void OnDataLoaded();

	// Short flavour of the original mod: NO explosion (removed on user
	// request).  Plays the turn-undead sound and teleports the body to the
	// vanilla dead-body cleanup cell so it vanishes.
	void ExecuteLayToRest(RE::FormID a_refID);

	// Bury with a Pay-Your-Respects gravestone + engraved memorial.
	// Shows a category message box, places PYR's grave activator at the
	// body, writes DeadName + Memorial onto its Papyrus script, then removes
	// the body.  DeadName uses GetDisplayName() so Real Names shows.
	void ExecuteBury(RE::FormID a_refID);

	// True if the player carries a vanilla shovel (Shovel01 0xF5D05 /
	// Shovel02 0xF5D06 in Skyrim.esm).  Gates the Bury prompt.
	bool PlayerHasShovel();

	// True if the ref is a dead actor that can be brought back (excludes ash
	// piles and non-actors).  Gates the Resurrect prompt.
	bool CanResurrect(RE::TESObjectREFR* a_ref);
	// An ash pile: an activator the engine links to the actor it came from
	// (ExtraAshPileRef). Never true for an actor, alive or dead.
	bool IsAshPile(RE::TESObjectREFR* a_ref);

	// A dead draugr this mod raised from a grave: never resurrected.
	bool IsRaisedRemains(RE::TESObjectREFR* a_ref);

	// Bring a dead actor back to life (RE::Actor::Resurrect).
	void ExecuteResurrect(RE::FormID a_refID);

	// True if the ref is a gravestone THIS mod placed (base == our grave
	// activator).  Used to show the Destroy Grave prompt only on our graves.
	bool IsOurGrave(RE::TESObjectREFR* a_ref);

	// Remove a gravestone this mod placed. A grave holding a unique NPC's
	// remains is dug up instead: the remains come back at the grave.
	void DestroyGrave(RE::FormID a_graveID);
	// True when the grave holds a unique NPC's remains (the prompt then reads Dig Up).
	bool GraveHasRemains(RE::FormID a_graveID);
	// New game / load: drop work that still holds FormIDs from the old game.
	void OnGameReset();
}
