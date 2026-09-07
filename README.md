# Bury & Take Bodies

A full-C++ SKSE mod (plugin identity `BuryTakeBodies`) that lets you deal with
corpses via SkyPrompt — lay to rest, bury with a custom-engraved gravestone,
resurrect, or pick the body up and carry it. Inspired by *Press F To Pay
Respects* and *Collect Bodies*, but fully independent (own plugin/DLL/theme, no
shared assets).

Look at a dead body and **hold Shift**: the loot list (QuickLoot IE) steps
aside and two prompts appear. Let go of Shift and everything is back to normal,
so a corpse behaves exactly as before until you ask for more.

- **Shift + F, tap → Lay to Rest** — turn-undead sound, body removed.
- **Shift + F, hold → Bury with Gravestone** — needs a shovel in your inventory
  (`requiresShovel`). Plays the shovel animation, then a text box asks for an
  **epitaph**; on confirm a gravestone (ships in `BuryTakeBodies.esp`) is
  placed at the body inscribed with the deceased's **display name** (Real
  Names / rename mods show correctly) + your message. Cancel the box (Esc) to
  leave the body untouched. The text box uses **UIExtensions**; without it the
  grave is placed with the name only.
- **Shift + E, tap → Resurrect** — brings the actor back (`resurrectResetInventory`
  decides whether they come back re-equipped or keep the looted inventory).
  Not offered on ash piles.
- **Shift + E, hold → Pick Up Body** *(dead humanoids only)* — carry the corpse
  as an inventory item named "<Name>'s body" (weight = body + its gear). Drop
  the item to set the body back down where you dropped it. This reimplements
  the *Collect Bodies* mod in C++ and fixes its three known bugs:
  1. **Names/weights no longer reset to "Collected Corpse"/1 after reload** —
     every carried corpse is mirrored into this plugin's SKSE cosave and
     re-applied on load.
  2. **Coexists with Hunterborn/harvest** — pick-up is a Shift-gated hold, not
     an activate-button takeover, so looting and harvesting still work.
  3. **Dremora (and other humanoids) can be collected** — the filter is the
     `ActorTypeNPC` keyword, not a race whitelist.
- **Destroy a grave** — look at a grave you placed, hold Shift, **hold E** to
  dig it out. Only affects graves this mod placed.

**QuickLoot IE** (3.x) is a soft dependency: while Shift is held on a corpse
the loot list is hidden through QuickLoot's own API so the E tap never also
takes an item; it returns the moment you release Shift or look away.

## Requirements

- **SkyPrompt** (the prompt framework)
- **UIExtensions** (soft) — provides the custom-epitaph text box for Bury.
  Almost every load order already has it. Without it, Bury still works but
  inscribes the name only (no message box).
- **PressFCorpses.esp** — ships in this mod folder. ESL-flagged, holding 50
  corpse-token MiscObjects + a hidden holding cell (pick-up) + the grave
  activator (bury). **Enable it in your plugin list** or both Pick Up and Bury
  stay disabled (Lay to Rest still works without it). No other plugins needed —
  Pay Your Respects is **not** required.
- Disable the original *Press F To Pay Respects* AND *Collect Bodies* — this
  replaces both.

## How pick-up works (internals)

Each of the 50 tokens is a distinct MiscObject so every carried body keeps its
own name + weight in the inventory. On pick-up the real body is teleported into
the never-rendered holding cell (`PFR_HoldingMarker`) and the matching token is
added to the player, renamed after the body. On drop, a `TESContainerChanged`
sink teleports the body back to the dropped token, deletes the token, and frees
the slot. Cap: 50 simultaneously-carried bodies.

## Config — `SKSE/Plugins/BuryTakeBodies.ini`

- `[General]` `shiftGatesPrompts` (default true — prompts only while the reveal
  modifier is held; false = always shown on a corpse), `hideQuickLootWhileRevealing`
  (default true — hide QuickLoot IE's loot list while the modifier is held).
- `[Hotkeys]` `layToRestKeyboard` (default 33 = F: tap Lay / hold Bury),
  `resurrectKeyboard` (default 18 = E: tap Resurrect / hold Take), plus gamepad
  variants (-1 = off). `buryKeyboard` / `collectKeyboard` are kept for old INIs
  but unused since the two-key layout.
- `[Bury]` `requiresShovel` (default true), `buryAnimation` (default true),
  `buryAnimationDelay` (seconds the dig plays, default 4).
- `[Resurrect]` `resurrectEnabled` (default true), `resurrectResetInventory`
  (default true — same as console `resurrect 1`).
- `[Grave]` `useDisplayName` (default true — engrave the runtime display name;
  false = base-object name like "Bandit"), `graveDestroyEnabled` (default true),
  `graveDestroyModifier` (default 42 = LShift, also the corpse reveal key),
  `graveDestroyKey` (default 18 = E).
- `[Collect]` `collectEnabled` (kept for old INIs; the Take hold is part of the
  E pair now).

## Build

```
cmake --preset release
cmake --build build/release --config Release
```
DLL auto-deploys to `X:/MODDINGSSE/modorganizer2/mods/Press F To Pay Respects--Claude/SKSE/Plugins`.

Depends on CommonLibSSE-NG (vcpkg, Monitor221hz registry). SimpleIni + the
SkyPrompt API header are vendored under `include/`.
