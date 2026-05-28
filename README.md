# OBS AirPlay Source

Native OBS input source for receiving iPhone/iPad AirPlay mirroring on macOS.

The plugin has one shared AirPlay receiver and any number of **AirPlay Video**
sources. Add the source multiple times to reuse the same live iPhone video in
different scenes or transforms.

## What It Does

- Creates one AirPlay host visible from iPhone Screen Mirroring.
- Registers an OBS custom browser dock named **AirPlay Receiver**.
- Adds an OBS source named **AirPlay Video**.
- Lets multiple source instances display the same shared receiver video.
- Decodes the RTP stream in-process with FFmpeg libraries.
- Uses VideoToolbox hardware decode by default.
- Outputs frames into OBS as an async video source.
- Defaults to `1920x1080@60`.
- Supports **H.264 / AVC** and **H.265 / HEVC** from the source settings.

## Build And Install

```bash
cd /Users/eklavya/Desktop/Airplay_Vcam/obs-plugin
./scripts/install.sh
```

Then restart OBS. The **AirPlay Receiver** dock appears under OBS docks/custom
browser docks, and the video source is:

```text
Sources -> + -> AirPlay Video
```

On the iPhone:

```text
Control Center -> Screen Mirroring -> OBS AirPlay
```

## Receiver Dock

The dock runs from:

```text
http://127.0.0.1:8799/
```

It shows receiver state, source count, and frame counters, with Start, Restart,
and Stop controls.

## Source Settings

- `AirPlay name`: The receiver name shown on iPhone.
- `UxPlay executable`: Defaults to `/Users/eklavya/Desktop/Airplay_Vcam/bin/uxplay`.
- `AirPlay codec`: Choose `H.264 / AVC` or `H.265 / HEVC`.
- `Width`, `Height`, `Frame rate`: Requested AirPlay mode and OBS frame output.
- `RTP port`: Local UDP port used between `uxplay` and the in-plugin decoder.
- `Use VideoToolbox hardware decode`: Best-performance decode path on macOS.

## Notes

H.265 requests HEVC AirPlay mode with `uxplay -h265` and uses an H.265 RTP SDP for
the in-plugin decoder. Device and network support can vary, so H.264 is the
safest default.

This plugin does not install or use a CoreMediaIO DAL virtual camera plugin. It
only shows the AirPlay stream as an OBS source.
