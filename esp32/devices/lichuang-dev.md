# Lichuang Szpi ESP32-S3 （立创·实战派）

Community port of the Muse ESP32 Device SDK to the LCKFB "Szpi"
ESP32-S3 development board (vendor board X9200-MB). Not an official
Meta profile: expect a debug round or two on real hardware.

## Hardware (verified from schematic SCH_X9200-MB-V1.0A and xiaozhi-esp32)

| Part | Detail |
|---|---|
| Module | ESP32-S3R8 (ESP32-S3-WROOM-1-N16R8): 16 MB flash, 8 MB octal PSRAM |
| Display | 2.0" ST7789 SPI, native 240x320 run as 320x240 landscape; SCK 41, MOSI 40, DC 39, backlight GPIO42 (inverted); chip select is PCA9557 P0, not a GPIO |
| Touch | FT6336 at I2C 0x38, polled (no reset/interrupt GPIO) |
| Audio out | ES8311 at 0x18 → NS4150B amp, amp enable is PCA9557 P1 |
| Audio in | ES7210 at 0x41 (note: not the 0x40 default); MIC1/MIC2 are the two MEMS mics, MIC3 is the speaker reference (unused by this port) |
| I2S | MCLK 38, BCLK 14, WS 13, DIN 12, DOUT 45 |
| I2C bus | SDA GPIO1, SCL GPIO2: codecs, touch, PCA9557 (0x19), QMI8658 IMU (0x6A), SY6302 gauge |
| Buttons | BOOT (GPIO0) = talk / pairing confirm; power key is a hardware latch (AK8052) |
| USB | Type-C → CH334F hub → S3 native USB Serial/JTAG (303a:1001) and CH340; both reach the chip |

## Build and flash

```sh
tools/muse/board.sh build lichuang
tools/muse/board.sh flash lichuang [PORT]
```

Set the SDK token in `build-muse-lichuang-dev/sdkconfig`
(`CONFIG_GADGET_SDK_TOKEN`) before building, or send it over serial
after flashing (`>sdk_token=mgst_...`).

## v1 limits

- No battery readout in Settings (SY6302 gauge not wired up yet).
- No camera (GC0308 on PCA9557 P2 power; the Muse UI here doesn't use it).
- No software power-off (hardware power key only).
- Touch transform follows xiaozhi's swap_xy + mirror_x; if taps land
  mirrored on your unit, flip the transform in
  `board_lichuang_dev.c`'s `touch_read_cb`.
- MIC3 reference / device-side AEC not used; voice takes MIC1.
