#pragma once

// On-device apps runtime ("local mode"): renders a rotation of built-in apps
// without a server. Configured with a JSON document stored in NVS:
//
// {
//   "timezone": "CET-1CEST,M3.5.0,M10.5.0/3",          POSIX TZ string
//   "brightness": {"day": 60, "night": 10, "night_start": "22:00", "night_end": "07:00"},
//   "display_off": {"enabled": true, "start": "00:00", "end": "06:00", "wake_minutes": 5},
//   "button": {"enabled": true, "tap": "next", "double_tap": "none", "hold": "display"},
//   "rotation": [
//     {"app": "clock", "dwell": 10, "enabled": true, "config": {"24h": true}},
//     ...
//   ]
// }

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

// True when the image URL selects local mode (empty or "local").
bool apps_local_mode(const char *image_url);

// Load the stored config. Call once at startup, before the API server.
void apps_init(void);

// Start the rendering task (local mode). Call once WiFi is up.
void apps_start(void);

// Current config as JSON (caller frees).
char *apps_get_config_json(void);
// Validate, store and apply a new config. On error returns ESP_ERR_INVALID_ARG
// and sets *error to a static message.
esp_err_t apps_set_config_json(const char *json, const char **error);

// Show a notification next (interrupts the current app).
esp_err_t apps_notify(const char *title, const char *text, const char *color,
                      int secs);
// Skip to the next app.
void apps_next(void);
// Button (Gen 2 touch bar), polled by the main task in local mode.
bool apps_button_enabled(void);
// True if a double tap has an action (single taps then wait for the window).
bool apps_button_double_tap(void);
// Run the action configured for "tap", "double_tap" or "hold".
void apps_button(const char *gesture);
// "on", "on (woken)", "off" or "off (schedule)".
const char *apps_display_state(void);
// Display on/off, like the button's "display" action (on wakes it during the
// display_off window for wake_minutes).
void apps_set_display(bool on);
// Brightness 0-100 until the next day/night switch.
void apps_set_brightness(int pct);
// Brightness currently sent to the display (0 when off).
int apps_brightness(void);
// Show the first enabled rotation item running `app` next.
esp_err_t apps_show(const char *app);
// Name of the app currently on screen ("" if none).
const char *apps_current(void);
// List of built-in app names, as a JSON array (caller frees).
char *apps_list_json(void);
