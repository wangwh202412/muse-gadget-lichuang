<!--
SPDX-License-Identifier: Apache-2.0

This repository is a third-party board port of Meta's muse-gadget-sdk
(Apache License 2.0). It is not an official Meta project and is not
affiliated with or endorsed by Meta Platforms, Inc. Upstream code and
documentation remain under their original Apache-2.0 license and
copyright notices (see LICENSE).
-->

# Muse Gadget on 立创·实战派 ESP32-S3 (lichuang-dev)

把 Meta 的 [muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)（ESP32 Device SDK，Apache-2.0）移植到**立创·实战派 ESP32-S3** 开发板（小智板名 `lichuang-dev`，board id `lichuang_dev`），并为语音留言回复接上火山豆包 TTS 朗读。本仓库是个人移植项目，与 Meta 无隶属或背书关系；英文说明见 [README-lichuang.md](README-lichuang.md)。

<p align="center">
  <img src="docs/images/lichuang-dev-1.png" width="430" alt="立创·实战派 ESP32-S3 开发板">
  <img src="docs/images/lichuang-dev-2.png" width="300" alt="立创·实战派 ESP32-S3 开发板（无线连接示意）">
</p>
<p align="center"><sub>立创·实战派 ESP32-S3 开发板产品图</sub></p>

## 板子

ESP32-S3R8（16 MB flash，8 MB octal PSRAM），320×240 ST7789 LCD（片选在 PCA9557 扩展器上，背光 GPIO42 反相），FT6336 触摸，ES8311 播放 codec + ES7210 双麦，GC0308 摄像头（本树未接入，见下方状态）。

## 相对上游的改动

- `esp32/components/muse/boards/board_lichuang_dev.c` —— 板级移植（显示、触摸、双 codec 音频、BOOT 键），并注册进 Kconfig / CMakeLists、`esp_lvgl_adapter` 组件规则和 `tools/muse/` 辅助映射。
- `esp32/devices/sdkconfig.muse-lichuang-dev` —— 构建 profile（`tools/muse/board.sh build lichuang`），开启实时语音。
- `esp32/components/muse/muse_chat_session.cpp` —— 把 `start_tts` 接到火山引擎豆包语音合成（v3 单向 HTTP，MP3 流式喂给自带解码器）：语音留言的回复会朗读出来。由 Kconfig 的 `MUSE_TTS_VOLCANO_API_KEY` / `_RESOURCE_ID` / `_SPEAKER` 配置。

其余均为上游原样文件（含上游自带的 README、图片与文档，版权与许可见 `LICENSE` 及各文件头）。

## 密钥——提交前必读

- 仓库里 overlay 的 `CONFIG_MUSE_TTS_VOLCANO_API_KEY=""` 为空。key 只填在本地副本，永远不要把填好的值提交上来。
- Gadget SDK token 不进代码树：刷机后经串口发送（`>sdk_token=...`），或写进构建目录的 sdkconfig（构建目录已被 gitignore）。

## 构建与刷机

需要 ESP-IDF **v6.0.3**（不多不少）。

```sh
cd esp32
tools/muse/board.sh build lichuang
tools/muse/board.sh flash lichuang /dev/cu.usbmodem112301   # 串口按实际设备改
```

配对：Muse app → Settings → Devices → Developer mode → Add Device → `MuseGadget-XXXXXX`，在设备上按键确认。板子与配对它的那个 Muse 账号通信。

## 状态（2026-11-23）

- 可用：显示、触摸、语音留言 + 朗读回复（火山 seed-tts-2.0，音色 小何2.0）、设置、经 app 配网。
- 实时语音通话：配对账号的 VM 以 licensing 错误拒绝 gadget 通话，固件侧无解。
- 拍照（GC0308）：后端已写过（`boards/lichuang_camera.c`，esp32-camera），2026-11-23 在硬件验证前暂存回滚，不在本树内。
