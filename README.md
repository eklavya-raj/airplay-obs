# 📱 AirPlay OBS Studio Plugin

![macOS Support](https://img.shields.io/badge/macOS-Apple_Silicon-blue?style=for-the-badge&logo=apple)
![OBS Support](https://img.shields.io/badge/OBS_Studio-29.0%2B-black?style=for-the-badge&logo=obsproject)
![License](https://img.shields.io/badge/License-MIT-green?style=for-the-badge)

A lightning-fast, production-ready OBS Studio input source plugin for receiving iPhone/iPad AirPlay mirroring on macOS. It decodes iOS screen casts in-process with zero-latency hardware acceleration and renders at a flawless 60 FPS.

## ✨ Features

- **Zero-Latency Video**: Custom drift-compensated monotonic timeline ensures frames stay perfectly synced without jitter.
- **Flawless Audio**: Reads the exact Presentation Timestamps (PTS) straight from the iOS device's hardware clock to eliminate micro-stutters and robotic artifacts.
- **O(1) Memory Pool**: Fully concurrent object-pooling architecture prevents garbage collection spikes and CPU bloat.
- **Glassmorphic UI**: Includes a beautiful, custom HTML/JS control dock injected right into OBS Studio.
- **Hardware Decoding**: Utilizes Apple's VideoToolbox for maximum performance and minimal battery drain on laptops.

## 🚀 Installation

We provide an automated installer package (`obs-airplay-installer.dmg`) in the [Releases](https://github.com/eklavya-raj/airplay-obs/releases) tab.

1. Download the latest `.dmg` installer.
2. Open it and double-click the `Install.command` script.
3. The script will safely bypass macOS Gatekeeper, sign the binaries, and configure the custom OBS dock automatically.
4. Restart OBS Studio!

## 🎮 Usage

1. Open OBS Studio.
2. In the "Sources" box, click the `+` button and add an **AirPlay Video** source.
3. The beautiful **AirPlay Receiver** control dock will appear. Make sure it says `Running`.
4. On your iPhone or iPad, open the Control Center and tap **Screen Mirroring**.
5. Select **OBS AirPlay** to start streaming your device's screen and audio directly into OBS at 60 FPS!

## 🛠️ Building from Source

Dependencies required: `Homebrew`, `cmake`, `ffmpeg`, `gstreamer`.

```bash
cd obs-plugin
./scripts/install.sh
```

## 📜 License
This project is open-source and free to use.
