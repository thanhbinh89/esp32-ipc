# ESP32-P4 IP Camera

ESP32-P4 IP camera firmware: captures 1080p H.264, runs on-device pedestrian
detection, and live-streams to a browser over WebRTC.

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
- Transport: WebRTC via libpeer, MQTT signaling, STUN/TURN ICE, DTLS-SRTP.

## Tasks

| Task        | Role                                    |
|-------------|-----------------------------------------|
| `video`     | capture, OSD, H.264 encode, send video  |
| `audio`     | G.711-A capture, send audio             |
| `ped_detect`| ESP-DL inference                        |
| `webrtc`    | `peer_signaling_loop` (MQTT)            |
| `peer`      | `peer_connection_loop` (ICE/DTLS/RTP)   |

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
idf.py reconfigure        # downloads managed_components/ from the registry
./patches/apply.sh        # re-apply local fixes to libpeer / srtp (see below)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Configure the MQTT signaling URL/token under
`idf.py menuconfig` -> **IPC Configuration**, or edit `CONFIG_SIGNALING_URL` /
`CONFIG_SIGNALING_TOKEN` in `sdkconfig`.

## Patched components (IMPORTANT)

`managed_components/` is **git-ignored** and re-downloaded from the ESP
component registry by the IDF component manager. It is **not** part of this
repo, so edits made there are lost on a fresh clone, after deleting
`managed_components/`, or whenever the manager re-fetches a component.

The required local fixes live as patch files in [`patches/`](patches/) and must
be re-applied after the components are fetched.

What the patches change:

- `patches/sepfy__libpeer.patch`
  - `src/config.h`: enable `CONFIG_DTLS_USE_ECDSA` (RSA-1024 keygen is too slow
    on the MCU and trips the watchdog).
  - `src/dtls_srtp.c`: DTLS authmode `VERIFY_OPTIONAL` instead of `REQUIRED` —
    WebRTC peers use self-signed certs, so the peer is verified by comparing the
    SDP `a=fingerprint` against the cert after the handshake, not via a CA chain.
  - `src/buffer.c`: allocate the media ring buffers in PSRAM
    (`heap_caps_calloc(MALLOC_CAP_SPIRAM)`) and null-check allocations; plain
    `calloc` only returns internal RAM here.
  - `src/ice.c`, `src/agent.c`: skip (instead of hard-fail on) non-UDP and
    unresolved mDNS ICE candidates so the rest of the SDP still parses.
  - `src/peer_connection.c`, `src/peer_signaling.c`: diagnostic logs around ICE
    gather / DTLS / signaling (timing + state).
- `patches/sepfy__srtp.patch`
  - `CMakeLists.txt`: silence `incompatible-pointer-types` (RISC-V `uint32_t` is
    `unsigned long`), otherwise the build fails.

### Applying the patches

```bash
idf.py reconfigure     # ensure managed_components/ exists
./patches/apply.sh
```

`apply.sh` is idempotent: it skips components already patched and only patches
pristine ones. Re-run it any time after a clone, `fullclean`, or a
`managed_components/` wipe. If a patch fails to apply, the registry version of
that component likely changed — regenerate the patch (see below).

### Regenerating a patch

After editing files under `managed_components/<component>/`, diff against the
pristine copy in the component-manager cache:

```bash
CACHE=~/.cache/Espressif/ComponentManager/service_*/    # pristine downloads
PRIS="$CACHE/sepfy__libpeer_0.0.3_<hash>"               # match dir for the component
diff -u --label a/src/foo.c --label b/src/foo.c \
  "$PRIS/src/foo.c" managed_components/sepfy__libpeer/src/foo.c \
  >> patches/sepfy__libpeer.patch
```

Patches are `-p1` relative to the component root (apply from inside
`managed_components/<component>/`). The pristine version+hash must match the one
pinned in `dependencies.lock`.
