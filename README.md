# Bury & Take Bodies

A full-C++ SKSE mod (plugin identity `BuryTakeBodies`) that lets you deal with
corpses via SkyPrompt — lay to rest, bury with a custom-engraved gravestone,
resurrect, or pick the body up and carry it. Inspired by *Press F To Pay
Respects* and *Collect Bodies*, but fully independent (own plugin/DLL/theme, no
shared assets).

Look at a dead body and **hold Shift**: the loot list (QuickLoot IE) steps
aside and two prompts appear. Let go of Shift and everything is back to normal,
so a corpse behaves exactly as before until you ask for more.

- **Shift + F, tap → Lay to Rest** — turn-undead sound, body removed. Quest
  items the body carried are handed to you first, so no quest gets stuck.
- **Shift + F, hold → Bury with Gravestone** — needs a shovel in your inventory
  (`requiresShovel`). Plays the shovel animation, then a text box asks for an
  **epitaph**; on confirm a gravestone (ships in `BuryTakeBodies.esp`) is
  placed at the body inscribed with the deceased's **display name** (Real
  Names / rename mods show correctly) + your message. Press Enter on an empty
  box and the grave reads "Here lies <Name>". Cancel the box (Esc) to
  leave the body untouched. The text box uses **UIExtensions**; without it (or if
  the box fails to open) the grave is placed with the name only. The headstone
  stands upright on the ground, turned toward you.
  - A **person** (not a creature) keeps their belongings, quest items too:
    everything they carried goes into
    their grave, and the grave's prompt reads **Dig Up** (Shift, hold E, needs a
    shovel). Digging brings up their remains: a dead draugr (the plain draugr
    body, male or female like the dead) called
    "Remains of <Name>" carrying everything they were buried with, lying in
    front of the grave with full ragdoll physics. Loot it like any body, carry
    it with Take Body, or Bury it again (a grave that can be dug up once more).
    It is never cleaned up and cannot be resurrected.
    Someone you buried as an ash pile comes back up as a small burial urn,
    "Ashes of <Name>", in front of the grave. You loot it like a body
    (QuickLoot shows what is inside, or open it), you can carry it with Pick Up
    (Shift, hold E) and set it down anywhere, and you can Bury it again (Shift,
    hold F). It stays an urn when empty. It has no physics (the game keeps
    containers fixed): it stands where it is set down.
  - Graves are permanent: they stay where you dug them.
  - A creature (even a unique horse or dragon): quest items come to you, the
    rest goes with the body. Loot first if you want the hide or the bones.
  - Lay to Rest leaves no grave: quest items always come to you.
- **Shift + E, tap → Resurrect** — brings the actor back (`resurrectResetInventory`
  decides whether they come back re-equipped or keep the looted inventory).
  Not offered on ash piles.
- **Shift + E, hold on an ash pile → Pick Up** — the ashes are gathered into a small
  burial urn, "Ashes of <Name>", holding what they carried (quest items come to
  you), and the urn goes into your inventory. Drop it to set the urn down where
  it lands (loot it, carry it again or bury it).
- **Shift + E, hold → Pick Up Body** *(anything dead — people, horses, dragons)* —
  carry the corpse as an inventory item named "<Name>'s body" (weight = body +
  its gear). Drop the item to set the body back down where you dropped it. You
  can hand a carried body to a follower: bodies don't count against their carry
  limit while you trade (`followersCarryBodies`). This reimplements
  the *Collect Bodies* mod in C++ and fixes its three known bugs:
  1. **Names/weights no longer reset to "Collected Corpse"/1 after reload** —
     every carried corpse is mirrored into this plugin's SKSE cosave and
     re-applied on load.
  2. **Coexists with Hunterborn/harvest** — pick-up is a Shift-gated hold, not
     an activate-button takeover, so looting and harvesting still work.
  3. **Dremora (and everything else) can be collected** — no race whitelist.
- **Edit Epitaph** — look at a grave you placed, hold Shift, **press F**: the
  epitaph box opens with the current epitaph in it. Enter re-engraves the grave
  (an empty box gives "Here lies <Name>"); Esc keeps the old inscription. Needs
  UIExtensions.
- **Destroy a grave** — look at a grave you placed, hold Shift, **hold E** to
  dig it out. Only affects graves this mod placed.

**QuickLoot IE** (3.x or 4.x) is a soft dependency: while Shift is held on a corpse
the loot list is hidden through QuickLoot's own API so the E tap never also
takes an item; it returns the moment you release Shift or look away.

## Requirements

- **SKSE** and **Address Library for SKSE Plugins**
- **SkyPrompt** (the prompt framework)
- **UIExtensions** (soft) — provides the custom-epitaph text box for Bury.
  Almost every load order already has it. Without it, Bury still works but
  inscribes the name only (no message box).
- **BuryTakeBodies.esp** — ships in this mod folder. ESL-flagged (header 1.70, so
  no BEES needed on older games), holding 50 corpse-token MiscObjects, a hidden
  holding cell (pick-up), the grave activator, the dig furniture and the
  "Remains" (male and female) and "Burial Urn" containers and the male and
  female "Remains" draugr (plus an unused "Ashes" item from an earlier design). **Enable it in your plugin list** or both Pick Up and Bury
  stay disabled (Lay to Rest still works without it). No other plugins needed —
  Pay Your Respects is **not** required.
- Disable the original *Press F To Pay Respects* AND *Collect Bodies* — this
  replaces both.
- Game versions: one DLL for SE 1.5.97, AE 1.6.x, AE 1.7.x (1.7.99 / 1.7.104) and VR
  through Address Library (1.7.x needs Address Library v13 or newer and SKSE 2.3.1+).
  Tested on AE 1.6.1170 (Steam) only. SE, GOG, 1.7.x and VR are untested.
- **Controller:** set `revealGamepad` (the button you hold, the controller's
  Shift), `layToRestGamepad` / `resurrectGamepad` (tap/hold like F / E) and
  `graveDestroyGamepad`. Codes: 266 D-Up, 267 D-Down, 268 D-Left, 269 D-Right,
  270 Start, 271 Back, 272 LS, 273 RS, 274 LB, 275 RB, 276 A, 277 B, 278 X, 279 Y,
  280 LT, 281 RT; -1 = off (the default). The held button is read straight from
  XInput, so a missed release can't leave the prompts stuck.

## How pick-up works (internals)

Each of the 50 tokens is a distinct MiscObject so every carried body keeps its
own name + weight in the inventory. On pick-up the real body is teleported into
the never-rendered holding cell (`PFR_HoldingMarker`) and the matching token is
added to the player, renamed after the body. On drop, a `TESContainerChanged`
sink teleports the body back to the dropped token, deletes the token, and frees
the slot. Cap: 50 simultaneously-carried bodies.

## Config — `SKSE/Plugins/BuryTakeBodies.ini`

The INI is not shipped: the plugin writes it with the commented defaults the first
time it runs (under MO2 it lands in `overwrite`), so an update never resets your values.

- `[General]` `shiftGatesPrompts` (default true — prompts only while the reveal
  modifier is held; false = always shown on a corpse), `hideQuickLootWhileRevealing`
  (default true — hide QuickLoot IE's loot list while the modifier is held).
- `[Hotkeys]` `layToRestKeyboard` (default 33 = F: tap Lay / hold Bury),
  `resurrectKeyboard` (default 18 = E: tap Resurrect / hold Take), plus gamepad
  variants and `revealGamepad` (-1 = off). `buryKeyboard` / `collectKeyboard` are kept for old INIs
  but unused since the two-key layout.
- `[Bury]` `requiresShovel` (default true), `buryAnimation` (default true),
  `buryAnimationDelay` (seconds the dig plays, default 4).
- `[Resurrect]` `resurrectEnabled` (default true), `resurrectResetInventory`
  (default true — same as console `resurrect 1`).
- `[Grave]` `useDisplayName` (default true — engrave the runtime display name;
  false = base-object name like "Bandit"), `graveDestroyEnabled` (default true),
  `graveDestroyModifier` (default 42 = LShift, also the corpse reveal key),
  `graveDestroyKey` (default 18 = E), `graveDestroyGamepad` (default -1).
- `[Collect]` `collectEnabled` (default true — false removes the Take hold),
  `followersCarryBodies` (default true — bodies weigh 0 while trading with a
  follower so they can be handed over).
- `[General]` `debug` (default false — true writes debug lines to the log).
- A key set to -1 on both keyboard and gamepad removes that prompt line.

## Changelog

**1.6.2**
- SkyPrompt 2.5.x: the grave prompts (Edit Epitaph, Dig Up, Destroy Grave) showed nothing and
  did nothing. They are now sent like the body prompts (anchored, a tap told from a hold):
  Shift + F opens the epitaph box, Shift + hold E digs up or destroys.
- Placed by the grave itself, the prompts went off screen or slid along the screen edge. They now sit
  on a small invisible marker just above the grave, removed again when the prompts close.
- The grave carries the headstone's real object bounds (they were empty).

**1.6.1**
- Skyrim 1.7.x (1.7.99 / 1.7.104): the DLL is now built on CommonLibSSE-NG 9.1.0, which
  reads the 1.7 Address Library and declares Address Library v5 support. 1.6.0 stopped
  at startup on 1.7.x with "REL/Relocation.h: failed to open address library file".
  No gameplay changes.

**1.6.0** (Nexus page versions before this: 1.3)
- Dig Up: a buried person comes back up as a dead draugr, "Remains of <Name>", male or
  female, carrying everything they were buried with (quest items and keys too). Loot it,
  carry it, or bury it again. Creatures keep no remains.
- Ash piles: Bury gives a grave that digs up as a small burial urn, "Ashes of <Name>";
  Pick Up (Shift, hold E) on an ash pile gathers the ashes into an urn and carries it.
  Urns can be looted, carried, set down and buried again. Twin ash piles of one actor
  leave together.
- Edit Epitaph on your graves (Shift, press F). An empty epitaph reads "Here lies <Name>";
  Tab cancels the box.
- Graves are permanent.
- Graves, dug-up remains and dropped urns sit on roads, floors and bridges instead of
  sinking to the terrain under them.
- Quest items always reach you (Lay to Rest, creatures, after the body has left).
- Bodies can be handed to followers (they don't count against the carry limit while you
  trade).
- QuickLoot IE 4.x support (the loot list hides while Shift is held).
- Controller: a reveal button (the controller's Shift) and a grave button.
- Safety: actions fire only on the body the key press began on; prompts never show on
  activation-blocked corpses (carry mods); the reveal key must be physically held;
  turned-off actions (Resurrect, Take) stay off.
- The INI is written on first run instead of shipped, so updates never reset settings.
- Fix: living NPCs with "ash" in their name no longer get the corpse prompts.

## Build

```
git submodule update --init --recursive --depth 1
build.bat
```
`build.bat` sets up the VS 2022 (14.44) environment and `VCPKG_ROOT=C:\vcpkg`, configures
`build\ng\release` on the first run (`cmake --preset release`) and builds. The DLL is copied
to `OUTPUT_FOLDER/SKSE/Plugins` (set `OUTPUT_FOLDER` in `CMakeLists.txt`).

CommonLibSSE-NG is the alandtse fork, tag v9.1.0, built from source out of the submodule
`extern/CommonLibSSE-NG` (vcpkg supplies fmt, spdlog, rapidcsv, DirectXTK). SimpleIni, the
SkyPrompt API and the QuickLoot API headers are vendored under `include/`.
