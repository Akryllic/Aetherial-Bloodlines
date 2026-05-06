# Aetherial Bloodlines

SKSE/CommonLibSSE NG plugin for race-based Health/Magicka/Stamina gain on level up.

This version uses the clean path:

- hook `ConfirmLevelUpAttributeCallback::Run`
- read the selected `ActorValue`
- set `iAVDhmsLevelup` to the race INI value
- call the original vanilla callback
- restore `iAVDhmsLevelup`

No menu-close cache. No after-the-fact add/subtract correction.

## INI

`Data/SKSE/Plugins/AetherialBloodlines.ini`
