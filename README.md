# ESP32-P4 IP Camera

ESP32-P4 IP camera firmware: captures 1080p H.264, runs on-device pedestrian
detection, sends G.711-A audio, and live-streams to a browser over WebRTC.

Built on ESP-IDF v5.5.4 and [libpeer](https://github.com/sepfy/libpeer)
(`sepfy/libpeer`) for the WebRTC stack.

📖 **Detailed docs:** [ARCHITECTURE.md](ARCHITECTURE.md) (blocks, tasks, boot & session sequences) · [DATAFLOW.md](DATAFLOW.md) (buffers, pixel formats, timing)

## Pipeline

```
OV5647 (MIPI-CSI) --> ISP (YUV420) --+--> PPA scale --> ESP-DL pedestrian detect
                                     |                        |
                                     |                   detection boxes
                                     v                        v
                              H.264 HW encoder <--------- OSD overlay
                                     |
                                     v
                          libpeer (RTP / SRTP / ICE)
                                     |
                                     v
                              browser (WebRTC)
```

- **Capture** — 1920x1080 packed YUV420 (`O_UYY_E_VYY`, not planar I420), sensor at
  30 fps, 3 mmap'd V4L2 buffers.
- **Detect** — PPA downscales each frame to 640x360 RGB565 (exactly 1/3 scale); the
  ESP-DL Pico s8 v1 model runs on core 1 at roughly 13 inferences/s.
- **Encode** — V4L2 m2m hardware H.264, ~1 Mbps target, QP 35–45, IDR every 5 frames,
  plus on-demand IDR on RTCP PLI.
- **Audio** — ES8311 over I2S, 16-bit mono 8 kHz, encoded as G.711-A (20 ms packets).
- **Transport** — WebRTC via libpeer: MQTT signaling, STUN/TURN ICE, DTLS-SRTP.

## Tasks

Everything runs on core 0 except the detector, which owns core 1.

| Task          | Core | Prio | Role                                      |
| ------------- | ---- | ---- | ----------------------------------------- |
| `video_cap` | 0    | 5    | V4L2 dequeue, push descriptors to a queue |
| `video_enc` | 0    | 5    | PPA feed, OSD, H.264 encode, send video   |
| `camera`    | 0    | 5    | device setup, then 1 Hz stats             |
| `audio`     | 0    | 4    | PCM read, G.711-A encode, send audio      |
| `detect`    | 1    | 7    | ESP-DL inference, box store               |
| `webrtc`    | 0    | 6    | `peer_signaling_loop` (MQTT)            |
| `peer`      | 0    | 5    | `peer_connection_loop` (ICE/DTLS/RTP)   |

## Prerequisites

ESP-IDF **v5.5.4** (esp_video requires IDF >= 5.3). Source the environment
before any `idf.py` command:

```bash
export IDF_PATH=/home/binh/.espressif/v5.5.4/esp-idf
export IDF_PYTHON_ENV_PATH=/home/binh/.espressif/tools/python/v5.5.4/venv
export IDF_PYTHON_CHECK_CONSTRAINTS=no
. $IDF_PATH/export.sh
```

## Build & Flash

```bash
idf.py set-target esp32p4
idf.py reconfigure        # downloads managed_components/ from registry
./patches/apply.sh        # re-apply local fixes to libpeer / srtp
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Runtime configuration

`idf.py menuconfig` — project options live in four menus:

| Menu                   | Contains                                                      |
| ---------------------- | ------------------------------------------------------------- |
| **IPC Internet** | Ethernet vs Wi-Fi choice, PHY/MDC/MDIO GPIOs, Wi-Fi SSID/pass |
| **IPC Wertc**    | Signaling URL + token, STUN URL, TURN enable/URL/credentials  |
| **IPC Features** | `APP_ENABLE_AI` — pedestrian detection on/off              |
| **IPC Video**    | MIPI-CSI SCCB I2C port/pins/frequency, sensor reset/pwdn pins |
| **IPC Audio**    | I2C/I2S port + pins, PA pin, sample rate, volume, mic gain    |

Notes:

- Network interface is a **compile-time** choice, not runtime. Wi-Fi goes through
  `esp_wifi_remote` / `esp_hosted` on a companion chip (the P4 has no radio).
- Turn `APP_ENABLE_AI` off to free internal DMA RAM — the esp-dl model is then never
  instantiated. Wi-Fi and AI together can exhaust internal RAM at boot.
- Full option tables with current values: [ARCHITECTURE.md §7](ARCHITECTURE.md#7-configuration-surface).

## Patched components

`managed_components/` is **git-ignored** and re-downloaded from the ESP component
registry by the IDF component manager. It is **not** part of this repo, so edits made
there are lost on a fresh clone, after deleting `managed_components/`, or whenever the
manager re-fetches a component.

Required local fixes live as patch files in [patches/](patches/) and must be re-applied
after components are fetched:

```bash
idf.py reconfigure     # ensure managed_components/ exists
./patches/apply.sh
```

`apply.sh` is idempotent: it skips components already patched and only patches pristine
ones. Re-run it any time after a clone, `fullclean`, or a `managed_components/` wipe.
If a patch fails to apply, the registry version likely changed — regenerate the patch.

The patches move libpeer's ring buffers to PSRAM, enable ECDSA DTLS with a 4 KB SDP
buffer, fix the RTP timestamp increment to 30 fps, harden fingerprint/candidate parsing,
and silence a RISC-V pointer-type warning in srtp. See
[ARCHITECTURE.md §9](ARCHITECTURE.md#9-dependencies-and-local-patches).

### Regenerating a patch

After editing files under `managed_components/<component>/`, diff against the pristine
copy in the component-manager cache:

```bash
CACHE=~/.cache/Espressif/ComponentManager/service_*/    # pristine downloads
PRIS="$CACHE/sepfy__libpeer_0.0.3_<hash>"               # match dir for component
diff -u --label a/src/foo.c --label b/src/foo.c \
  "$PRIS/src/foo.c" managed_components/sepfy__libpeer/src/foo.c \
  >> patches/sepfy__libpeer.patch
```

Patches are `-p1` relative to component root (apply from inside
`managed_components/<component>/`). The pristine version+hash must match the one pinned
in `dependencies.lock`.

## Known gaps

The current working tree has a few open defects — most importantly the detector feed
pipeline is passed to `video_task` before it is allocated, so detection is inert. Full
list: [ARCHITECTURE.md §10](ARCHITECTURE.md#10-known-gaps) and
[DATAFLOW.md §10](DATAFLOW.md#10-data-flow-defects).
