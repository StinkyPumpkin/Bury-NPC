#pragma once

namespace PickupManager
{
	// Registered at plugin load — installs the SKSE cosave callbacks so
	// carried-corpse names/weights survive save/reload (fixes the original
	// "resets to Collected Corpse / weight 1" bug).
	void RegisterSerialization();

	// Called on kDataLoaded — enumerates the corpse MiscObject slots + the
	// holding-cell marker from PressFCorpses.esp, caches ActorTypeNPC, and
	// installs the container-changed sink used to detect drops.
	void Init();

	// True for a dead humanoid actor (ActorTypeNPC — includes Dremora, unlike
	// the original mod).  Used to decide whether to show the pick-up prompt.
	bool CanCollect(RE::TESObjectREFR* a_ref);

	// Pick the body up: name a free corpse token after it, stash the body in
	// the holding cell, and add the token to the player.
	void ExecuteCollect(RE::FormID a_refID);
}
