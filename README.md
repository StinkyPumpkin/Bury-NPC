# Press F To Pay Respects — Claude fork

A full-C++ SKSE fork of *Press F To Pay Respects* that adds a **bury option**
and a **pick-up-body option** via SkyPrompt. Look at any dead body (or ash
pile) and:

- **Hold F → Lay to Rest** — plays the turn-undead sound and removes the body
  (no explosion — removed from the original).
- **Hold R → Bury with Gravestone** — pick a memorial category (Friend /
  Stranger / Foe), places a *Pay Your Respects* gravestone at the body engraved
  with the deceased's **display name** (Real Names / any rename mod shows
  correctly) and a memorial line, then removes the body.
- **Hold T → Pick Up Body** *(dead humanoids only)* — carry the corpse as an
  inventory item named "<Name>'s body" (weight = body + its gear). Drop the
  item to set the body back down where you dropped it. This reimplements the
  *Collect Bodies* mod in C++ and fixes its three known bugs:
  1. **Names/weights no longer reset to "Collected Corpse"/1 after reload** —
     every carried corpse is mirrored into this plugin's SKSE cosave and
     re-applied on load.
  2. **Coexists with Hunterborn/harvest** — pick-up is a separate hold key, not
     an activate-button takeover, so looting and harvesting still work.
  3. **Dremora (and other humanoids) can be collected** — the filter is the
     `ActorTypeNPC` keyword, not a race whitelist.

## Requirements

- **SkyPrompt** (the prompt framework)
- **Pay Your Respects SSE** — provides the grave activator + engraving script
  that the Bury option reuses. Its own MCM / perk-activation are not used by
  this fork; only the grave form is borrowed.
- **PressFCorpses.esp** — ships in this mod folder. It is an ESL-flagged plugin
  holding 50 corpse-token MiscObjects + a hidden holding cell used by the
  pick-up feature. **Enable it in your plugin list** or pick-up stays disabled
  (Lay/Bury still work without it).
- Disable the original *Press F To Pay Respects* AND *Collect Bodies* — this
  replaces both.

## How pick-up works (internals)

Each of the 50 tokens is a distinct MiscObject so every carried body keeps its
own name + weight in the inventory. On pick-up the real body is teleported into
the never-rendered holding cell (`PFR_HoldingMarker`) and the matching token is
added to the player, renamed after the body. On drop, a `TESContainerChanged`
sink teleports the body back to the dropped token, deletes the token, and frees
the slot. Cap: 50 simultaneously-carried bodies.

## Config — `SKSE/Plugins/PressFtoPayRespects.ini`

- `[Hotkeys]` `layToRestKeyboard` (default 33 = F), `buryKeyboard`
  (default 19 = R), `collectKeyboard` (default 20 = T), plus gamepad variants
  (-1 = off).
- `[Grave]` `useDisplayName` (default true — engrave runtime name),
  `includePlayerName` (default true — "Buried by <player>").
- `[Collect]` `collectEnabled` (default true — set false to hide the pick-up
  prompt).

## Build

```
cmake --preset release
cmake --build build/release --config Release
```
DLL auto-deploys to `X:/MODDINGSSE/modorganizer2/mods/Press F To Pay Respects--Claude/SKSE/Plugins`.

Depends on CommonLibSSE-NG (vcpkg, Monitor221hz registry). SimpleIni + the
SkyPrompt API header are vendored under `include/`.
