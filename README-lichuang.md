# Muse Gadget on 立创·实战派 ESP32-S3 (lichuang-dev)

A board port of Meta's [muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(ESP32 Device SDK, Apache-2.0) for the LCKFB 立创·实战派 ESP32-S3 development
board (xiaozhi board name `lichuang-dev`, board id `lichuang_dev`), plus a
spoken-reply TTS wiring. Everything else is upstream.

## Board

ESP32-S3R8 (16 MB flash, 8 MB octal PSRAM), 320x240 ST7789 LCD (CS on a
PCA9557 expander, backlight GPIO42 inverted), FT6336 touch, ES8311 playback
codec + ES7210 dual mic, GC0308 camera (not wired in this tree, see Status).

## Changes vs upstream

- `esp32/components/muse/boards/board_lichuang_dev.c` — the board port
  (display, touch, dual-codec audio, BOOT button), registered as
  `MUSE_BOARD_LICHUANG_DEV` in Kconfig/CMakeLists, the esp_lvgl_adapter
  component rule, and the `tools/muse/` helper maps.
- `esp32/devices/sdkconfig.muse-lichuang-dev` — build profile
  (`tools/muse/board.sh build lichuang`), with realtime voice enabled.
- `esp32/components/muse/muse_chat_session.cpp` — `start_tts` wired to
  Volcano Engine Doubao TTS (v3 unidirectional HTTP, MP3 streamed into the
  stock decoder): voice-note replies are spoken aloud. Configured by
  `MUSE_TTS_VOLCANO_API_KEY` / `_RESOURCE_ID` / `_SPEAKER` in Kconfig.

## Secrets — read before committing

- The committed overlay ships with `CONFIG_MUSE_TTS_VOLCANO_API_KEY=""`.
  Fill the key only in your local copy; never commit a filled value.
- The Gadget SDK token is never in this tree: send it over serial after
  flashing (`>sdk_token=...`), or set it in the build dir's sdkconfig
  (build dirs are gitignored).

## Build & flash

Requires ESP-IDF **v6.0.3** exactly.

```sh
cd esp32
tools/muse/board.sh build lichuang
tools/muse/board.sh flash lichuang /dev/cu.usbmodem112301   # port varies
```

Pairing: Muse app → Settings → Devices → Developer mode → Add Device →
`MuseGadget-XXXXXX`, confirm with a BOOT press. The board talks to the
Muse account it is paired with.

## Status (2026-11-23)

- Working: display, touch, voice notes with spoken replies (Volcano
  seed-tts-2.0, speaker 小何2.0), settings, Wi-Fi provisioning via the app.
- Realtime voice calls: the paired account's VM rejects gadget calls with
  a licensing error; not fixable from the firmware side.
- Photo notes (GC0308): a backend was written
  (`boards/lichuang_camera.c`, esp32-camera) and then parked on
  2026-11-23 before hardware verification; it is not in this tree.
