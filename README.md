# Press F To Pay Respects — Claude fork

A full-C++ SKSE fork of *Press F To Pay Respects* that adds a **bury option**
via SkyPrompt. Look at any dead body (or ash pile) and:

- **Hold F → Lay to Rest** — plays the turn-undead sound and removes the body
  (no explosion — removed from the original).
- **Hold R → Bury with Gravestone** — pick a memorial category (Friend /
  Stranger / Foe), places a *Pay Your Respects* gravestone at the body engraved
  with the deceased's **display name** (Real Names / any rename mod shows
  correctly) and a memorial line, then removes the body.

## Requirements

- **SkyPrompt** (the prompt framework)
- **Pay Your Respects SSE** — provides the grave activator + engraving script
  that this reuses. Its own MCM / perk-activation are not used by this fork;
  only the grave form is borrowed.
- Disable the original *Press F To Pay Respects* — this replaces it (same DLL
  name).

## Config — `SKSE/Plugins/PressFtoPayRespects.ini`

- `[Hotkeys]` `layToRestKeyboard` (default 33 = F), `buryKeyboard`
  (default 19 = R), plus gamepad variants (-1 = off).
- `[Grave]` `useDisplayName` (default true — engrave runtime name),
  `includePlayerName` (default true — "Buried by <player>").

## Build

```
cmake --preset release
cmake --build build/release --config Release
```
DLL auto-deploys to `X:/MODDINGSSE/modorganizer2/mods/Press F To Pay Respects--Claude/SKSE/Plugins`.

Depends on CommonLibSSE-NG (vcpkg, Monitor221hz registry). SimpleIni + the
SkyPrompt API header are vendored under `include/`.
