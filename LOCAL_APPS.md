# Local apps

This branch (`local-apps`) lets the Tidbyt render its apps itself — no server.
With the image URL set to `local` (or empty) the device runs a rotation of
built-in apps; any other URL keeps the normal Tronbyt server behaviour.

| App | Data source | Notes |
|---|---|---|
| `clock` | SNTP | blinking colon, `24h` option |
| `weather` | Open-Meteo | `latitude`, `longitude` (°C) |
| `departures` | Transitous (DELFI incl. HVV realtime) | `stop_id`, `platform_ids`, `lines`, `rename`, `rows` |
| `spotify` | Spotify Web API | needs a one-time login, skipped while nothing plays |
| `message` | config | static `title`/`text`/`color` |

Notifications (`POST /api/notify`) interrupt the rotation.

## Display and button

Set on the settings page (or in the config JSON):

```json
"brightness":  {"day": 60, "night": 10, "night_start": "22:00", "night_end": "07:00"},
"display_off": {"enabled": true, "start": "00:00", "end": "06:00", "wake_minutes": 5},
"button":      {"enabled": true, "tap": "next", "double_tap": "none", "hold": "display"}
```

- **Brightness** follows the day/night times. Whole hours (`22`) still work.
- **Display off** blanks the screen and stops all apps (no data fetching) in
  that window. A button press wakes it for `wake_minutes`.
- **Button** (Gen 2 touch bar) actions: `none`, `next`, `display` (on/off),
  `brightness` (10 → 25 → 50 → 75 → 100 %, until the next day/night switch) and
  `show:<app>` (jump to that app, e.g. `show:departures`). While the display is
  off, any gesture with an action turns it on. Turning it off with the button
  lasts until the end of the next display-off window (or until pressed again).
  Single taps react immediately unless a double tap action is set. The button
  is off by default on Gen 2 (spurious touches), so enable it here.

## Settings page and API (port 8080)

Open `http://<device-ip>:8080/` for the settings page. The same JSON API is
meant for the Android app:

```
GET  /api/status            firmware, mode, IP, current app, display state
GET  /api/apps              built-in app names
GET  /api/config            {"timezone", "brightness", "rotation": [...]}
PUT  /api/config            replace (validated, stored in NVS)
POST /api/notify            {"text", "title", "color", "secs"}
POST /api/next
POST /api/display         {"on": true|false}
POST /api/brightness      {"value": 0-100}   until the next day/night switch
POST /api/show            {"app": "departures"}
POST /api/spotify           {"client_id", "refresh_token"}   DELETE to clear
POST /api/mode              {"image_url": "local" | "http://..."}  (reboots)
POST /api/ota               raw firmware.bin                 (reboots)
POST /api/reboot
```

There is no authentication — anyone on the LAN can change settings or upload
firmware.

## Build

Requires [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/).
After running its export script (`. $IDF_PATH/export.sh`, or `export.ps1` on Windows):

```bash
idf.py -B build-apps -D SDKCONFIG=build-apps/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tidbyt-gen2" set-target esp32   # once
idf.py -B build-apps -D SDKCONFIG=build-apps/sdkconfig build
```

## Update the device

Over WiFi (writes the inactive OTA slot; the image is SHA-256-checked before
the device switches to it):

```bash
curl -X POST http://<device-ip>:8080/api/ota --data-binary @build-apps/firmware.bin
```

Over USB, app only (keeps bootloader, partition table and WiFi settings):

```bash
python -m esptool --port <port> -b 460800 write-flash 0x10000 build-apps/firmware.bin
```

After a USB write to `0x10000`, make sure the device boots that slot: if
`/api/status` says `"partition": "app1"`, also write `build-apps/ota_data_initial.bin`
at `0xe000`. Use 460800 baud — 921600 is unreliable with the CH340 on the
Tidbyt Gen 2.

Before flashing anything, back up the original flash
(`python -m esptool --port <port> read-flash 0 ALL tidbyt-backup.bin`). The
backup contains your WiFi credentials, so keep it private.

## Adding an app

1. Create `main/app_<name>.c` with `prepare` (fetch data, return false to skip)
   and `draw` (one frame into the 64x32 canvas; return how long the content
   needs, e.g. a marquee pass). Use `http_util.h` for HTTPS and the `cfg_*`
   helpers for its config.
2. Declare it in `app_api.h`, register it in `APPS[]` in `apps.c`, add the
   source to `main/CMakeLists.txt` and a config template in `main/web/index.html`.

Fonts are generated from the BDF files with `python tools/fonts2c.py <dir>`.
`test/host/` renders reference scenes with the device code on the PC (via
`python -m ziglang cc`) and diffs them against the Python tidbyt-server.

## Spotify

Needs Spotify Premium (Development Mode requirement since Feb 2026).

1. Create an app at https://developer.spotify.com/dashboard, add the redirect
   URI `http://127.0.0.1:8888/callback`, enable "Web API".
2. `python tools/spotify_auth.py --device <device-ip> --client-id <id>`
