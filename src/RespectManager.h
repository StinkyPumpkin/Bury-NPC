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

	// True if the ref is a gravestone THIS mod placed (base == our grave
	// activator).  Used to show the Destroy Grave prompt only on our graves.
	bool IsOurGrave(RE::TESObjectREFR* a_ref);

	// Remove a gravestone this mod placed.
	void DestroyGrave(RE::FormID a_graveID);
}
