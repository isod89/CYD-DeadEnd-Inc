# CYD-DeadEnd-Inc

Dead End Inc. is a compact touchscreen management game built for the Cheap Yellow Display (`ESP32-2432S028` / CYD). You build a fragile fictional street network, manage cash, debt, heat, districts, contacts, lab progression, and campaign objectives, while the SD card holds the world and mod packs.

## What It Does

- dark, CYD-friendly UI with large touch targets
- automatic in-game clock with slow passive time flow
- actions consume time, so production and deliveries feel alive
- passive heat cooldown over time, so waiting is a real survival option
- fictional products with `LAB` and `REP` requirements
- districts, contacts, events, and campaign progression
- bust system with meaningful penalties
- persistent save on SD card
- mod-aware content loading from SD
- extra SD packs for stress testing and long campaigns

## Hardware

- `CYD ESP32-2432S028`
- `320x240` touchscreen
- SD card support
- built with PlatformIO

## Project Layout

- [platformio.ini](platformio.ini)
- [src/main.cpp](src/main.cpp)
- [include/User_Setup.h](include/User_Setup.h)
- [lib/CYD-touch/CYD28_TouchscreenR.h](lib/CYD-touch/CYD28_TouchscreenR.h)

SD content:

- default pack: [sdcard/deadend](sdcard/deadend)
- longer campaign pack: [sdcard/hours_campaign_pack](sdcard/hours_campaign_pack)
- dense content pack: [sdcard/dense_campaign_pack](sdcard/dense_campaign_pack)
- limit test pack: [sdcard/limit_test_pack](sdcard/limit_test_pack)

## Build

From the repo root:

```powershell
C:\Users\Sam\.platformio\penv\Scripts\platformio.exe run -e esp32-2432s028r
```

## Flash

Example long flash command on `COM7`:

```powershell
py -m esptool --chip esp32 --port COM7 erase-flash
py -m esptool --chip esp32 --port COM7 --baud 460800 write-flash -z `
0x1000 ".pio\build\esp32-2432s028r\bootloader.bin" `
0x8000 ".pio\build\esp32-2432s028r\partitions.bin" `
0x10000 ".pio\build\esp32-2432s028r\firmware.bin"
```

## SD Card

Copy one of the `deadend` folders to the root of the SD card as:

```text
/deadend/
```

If you want to keep an existing run, preserve:

```text
/deadend/saves/
```

## Modding

The engine loads base content plus optional files dropped into:

```text
/deadend/mods/products/
/deadend/mods/districts/
/deadend/mods/characters/
/deadend/mods/events/
/deadend/mods/campaign/
```

The current product format supports:

```text
id|label|basePrice|risk|cookCost|batchYield|heatGain|requiredLab|requiredRep|desc
```

Older product files without `requiredLab` and `requiredRep` still work. The firmware derives defaults automatically.

More format details are documented in:

- [sdcard/deadend/system/README.txt](sdcard/deadend/system/README.txt)

## Notes

- This repo keeps the source and SD packs. Build output is ignored.
- The game is designed around short sessions, but the long campaign pack stretches progression much further.
