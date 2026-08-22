# Aetherial Bloodlines

A race overhaul for Skyrim SE/AE. SKSE plugin plus a light plugin, built on CommonLibSSE-NG.

Developers: ItzIvy, Apodeictic

Every race trades a strength for a weakness, and each has an ability that answers its own drawback.
Most of the behaviour that used to live in Papyrus now runs in C++.

## What it does

- **Per-race attribute gains on level up.** Vanilla gives +10 of whichever attribute you pick. Each
  race gets its own spread instead.
- **Racial passives and drawbacks**, on records rather than scripts wherever the engine allows it. One
  toggle turns every drawback off, and the abilities that pay for them go with it.
- **Triggered racial abilities.** No Powers menu. You do something in ordinary play and the ability
  answers.
- **The Green Pact.** A Bosmer cannot harvest plants, and the refusal shows on the crosshair before
  you press rather than after.
- **Configuration in game** through SKSE Menu Framework, on Settings and Level Up pages. Both write
  straight back to the INI, so a modlist author can set a list up from the menu and ship the result.

## Requirements

- SKSE
- Address Library for SKSE Plugins
- powerofthree's Tweaks, for editor ID lookup at runtime
- Actor Value Generator, for the per-actor Orsimer and Bosmer counters
- SKSE Menu Framework, optional. Without it everything works, configured by INI
- Precision, optional. With it Bloodthirst scales on the damage a blow deals rather than on max health

## Configuration

`Data/SKSE/Plugins/AetherialBloodlines.ini`.

`[Settings]` holds what a new game starts each menu option at. Changing an option in game affects the
current save only. `[Default]` covers any race the file does not name, including modded ones, and
adding a section named after a race's EditorID makes it appear on the Level Up page at the next load.

## Building

```
cmake --preset release
cmake --build build/release
```

Needs CMake, vcpkg with `VCPKG_ROOT` set, and CommonLibSSE-NG with `COMMONLIB_SSE_FOLDER` set.

## Credits

- [CharmedBaryon](https://github.com/CharmedBaryon/CommonLibSSE-NG) and
  [alandtse](https://github.com/alandtse/CommonLibSSE-NG) for CommonLibSSE-NG
- [powerofthree](https://www.nexusmods.com/skyrimspecialedition/mods/51073) for po3's Tweaks
- [NoahBoddie](https://www.nexusmods.com/skyrimspecialedition/mods/84743) for Actor Value Generator
- [Thiago099](https://www.nexusmods.com/skyrimspecialedition/mods/120352) for SKSE Menu Framework
- [Ersh](https://www.nexusmods.com/skyrimspecialedition/mods/72347) for Precision
