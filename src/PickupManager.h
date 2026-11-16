#pragma once

namespace PickupManager
{
	// Registered at plugin load — installs the SKSE cosave callbacks so
	// carried-corpse names/weights survive save/reload (fixes the original
	// "resets to Collected Corpse / weight 1" bug).
	void RegisterSerialization();

	// Called on kDataLoaded — enumerates the corpse MiscObject slots + the
	// holding-cell marker from BuryTakeBodies.esp, caches ActorTypeNPC, and
	// installs the container-changed sink used to detect drops.
	void Init();

	// Pick the body up: name a free corpse token after it, stash the body in
	// the holding cell, and add the token to the player.
	void ExecuteCollect(RE::FormID a_refID);
	// A carry slot is free (up to 50 bodies / urns at once).
	bool HasFreeSlot();

	// The persistent marker in our holding cell (BuryTakeBodies.esp 0x801), or null.
	RE::TESObjectREFR* GetHoldingMarker();

	// Buried unique NPCs: the body waits in the holding cell and can be dug up
	// from its grave. Saved in the cosave with the carried bodies.
	void RegisterBuriedBody(RE::FormID a_graveID, RE::FormID a_bodyID);
	bool HasBuriedBody(RE::FormID a_graveID);
	// Returns the body and forgets the link (0 when the grave holds none).
	RE::FormID TakeBuriedBody(RE::FormID a_graveID);
}
