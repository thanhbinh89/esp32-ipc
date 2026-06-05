# ESP32-P4 IP Camera

ESP32-P4 IP camera firmware: captures 1080p H.264, runs on-device pedestrian
detection, sends G.711-A audio, and live-streams to a browser over WebRTC.

Built on ESP-IDF v5.5.4 and [libpeer](https://github.com/sepfy/libpeer)
(`sepfy/libpeer`) for the WebRTC stack.

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

- Capture: 1920x1080 YUV420, sensor at 30fps, frame pacing via `FRAME_SKIP` in `main/video_task.cpp`.
- Detect: PPA downscales each frame to RGB565, ESP-DL pedestrian model runs on core 1.
- Encode: V4L2 m2m hardware H.264, ~1 Mbps target, QP 28-42.
- Audio: ES8311 over I2S, encoded as G.711-A for WebRTC.
- Transport: WebRTC via libpeer, MQTT signaling, STUN/TURN ICE, DTLS-SRTP.

## Tasks

| Task         | Role                                   |
|--------------|----------------------------------------|
| `video`      | capture, OSD, H.264 encode, send video |
| `audio`      | G.711-A capture, send audio            |
| `ped_detect` | ESP-DL inference                       |
| `webrtc`     | `peer_signaling_loop` (MQTT)           |
| `peer`       | `peer_connection_loop` (ICE/DTLS/RTP)  |

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

Open project config:

```bash
idf.py menuconfig
```

Use **IPC Configuration**:

- **Signaling URL** (`CONFIG_SIGNALING_URL`): MQTT/signaling endpoint passed to `peer_signaling_connect()`.
- **Signaling Token** (`CONFIG_SIGNALING_TOKEN`): token passed to `peer_signaling_connect()`.
- **STUN URL** (`CONFIG_STUN_URL`): first ICE server. Default: `stun.l.google.com:19302`.
- **Enable TURN** (`CONFIG_TURN`): enable only when STUN alone cannot connect through NAT/firewall.
- **TURN URL** (`CONFIG_TURN_URL`): second ICE server. Default: `turn:openrelay.metered.ca:80`.
- **TURN Username** (`CONFIG_TURN_USERNAME`): TURN auth username.
- **TURN Credential** (`CONFIG_TURN_CREDENTIAL`): TURN auth credential/password.

## Patched components

`managed_components/` is **git-ignored** and re-downloaded from the ESP
component registry by the IDF component manager. It is **not** part of this
repo, so edits made there are lost on a fresh clone, after deleting
`managed_components/`, or whenever the manager re-fetches a component.

Required local fixes live as patch files in [patches/](patches/) and must be
re-applied after components are fetched.

### Applying patches

```bash
idf.py reconfigure     # ensure managed_components/ exists
./patches/apply.sh
```

`apply.sh` is idempotent: it skips components already patched and only patches
pristine ones. Re-run it any time after a clone, `fullclean`, or a
`managed_components/` wipe. If a patch fails to apply, registry version likely
changed — regenerate patch.

### Regenerating patch

After editing files under `managed_components/<component>/`, diff against the
pristine copy in component-manager cache:

```bash
CACHE=~/.cache/Espressif/ComponentManager/service_*/    # pristine downloads
PRIS="$CACHE/sepfy__libpeer_0.0.3_<hash>"               # match dir for component
diff -u --label a/src/foo.c --label b/src/foo.c \
  "$PRIS/src/foo.c" managed_components/sepfy__libpeer/src/foo.c \
  >> patches/sepfy__libpeer.patch
```

Patches are `-p1` relative to component root (apply from inside
`managed_components/<component>/`). Pristine version+hash must match the one
pinned in `dependencies.lock`.
