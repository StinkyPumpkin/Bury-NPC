# Bury & Take Bodies

A full-C++ SKSE mod (plugin identity `BuryTakeBodies`) that lets you deal with
corpses via SkyPrompt — lay to rest, bury with a custom-engraved gravestone, or
pick the body up and carry it. Inspired by *Press F To Pay Respects* and
*Collect Bodies*, but fully independent (own plugin/DLL/theme, no shared assets).
Look at any dead body (or ash pile) and:

- **Hold F → Lay to Rest** — plays the turn-undead sound and removes the body
  (no explosion — removed from the original).
- **Hold R → Bury with Gravestone** — a text box pops up to type a **custom
  epitaph**; on confirm, a gravestone (ships in `PressFCorpses.esp`) is placed
  at the body inscribed with the deceased's **display name** (Real Names / any
  rename mod shows correctly) + your message, then the body is removed. The
  inscription is the grave's crosshair name. Cancel the box (Esc) to leave the
  body untouched. The text box uses **UIExtensions** (near-universal); without
  it, the grave is placed with the name only.
- **Destroy a grave** — look at a grave you placed, **hold Shift** (the reveal
  modifier) and a "Destroy Grave" hold-prompt appears; **hold E** to dig it out
  (Shift+E). The prompt stays hidden until you hold Shift, so it never clutters
  the epitaph. Only affects graves this mod placed. Keys configurable
  (`graveDestroyModifier` / `graveDestroyKey`).
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

- `[Hotkeys]` `layToRestKeyboard` (default 33 = F), `buryKeyboard`
  (default 19 = R), `collectKeyboard` (default 20 = T), plus gamepad variants
  (-1 = off).
- `[Grave]` `useDisplayName` (default true — engrave the runtime display name;
  false = base-object name like "Bandit").
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
