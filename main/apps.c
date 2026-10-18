#include "apps.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_api.h"
#include "display.h"
#include "gfx.h"

static const char *TAG = "apps";

#define NVS_NS "apps"
#define NVS_KEY "config"
#define FRAME_MS 40
#define MAX_SHOW_MS 60000  // cap for long marquees / notifications
#define TASK_STACK 12288  // TLS handshakes + JSON parsing run in this task

extern volatile int32_t isAnimating;  // gfx.c animation state (main.c)

static const app_t *const APPS[] = {&app_clock, &app_weather, &app_departures,
                                    &app_spotify, &app_message};
#define APP_COUNT (sizeof(APPS) / sizeof(APPS[0]))

#ifdef CONFIG_DISABLE_TOUCH_DEFAULT
#define BUTTON_DEFAULT_ENABLED false
#else
#define BUTTON_DEFAULT_ENABLED true
#endif
#define WAKE_MINUTES_DEFAULT 5
#define ACTION_LEN 32

// Brightness steps for the "brightness" button action.
static const int BRIGHTNESS_STEPS[] = {10, 25, 50, 75, 100};
#define BRIGHTNESS_STEP_COUNT (sizeof(BRIGHTNESS_STEPS) / sizeof(BRIGHTNESS_STEPS[0]))

static const char *DEFAULT_CONFIG =
    "{\"timezone\":\"CET-1CEST,M3.5.0,M10.5.0/3\","
    "\"brightness\":{\"day\":60,\"night\":10,\"night_start\":\"22:00\",\"night_end\":\"07:00\"},"
    "\"display_off\":{\"enabled\":false,\"start\":\"00:00\",\"end\":\"06:00\",\"wake_minutes\":5},"
    "\"rotation\":["
    "{\"app\":\"clock\",\"dwell\":10,\"enabled\":true,\"config\":{\"24h\":true}},"
    "{\"app\":\"weather\",\"dwell\":10,\"enabled\":false,"
    "\"config\":{\"latitude\":null,\"longitude\":null}},"
    "{\"app\":\"spotify\",\"dwell\":10,\"enabled\":true,\"config\":{}}"
    "]}";

typedef struct {
  char title[32];
  char text[256];
  char color[10];
  int secs;
} notification_t;

typedef struct {
  bool enabled;
  char tap[ACTION_LEN];
  char double_tap[ACTION_LEN];
  char hold[ACTION_LEN];
} button_t;

static SemaphoreHandle_t s_lock;
static char *s_config_json;  // stored config, guarded by s_lock
static volatile bool s_config_changed;
static QueueHandle_t s_notifications;
static volatile bool s_skip;
static const char *volatile s_current = "";
static TaskHandle_t s_task;
static int s_brightness = -1;  // last value sent to the display

// Display state. The apps task owns the schedule; button presses (main task)
// only set these flags and let the apps task apply them.
static button_t s_button;  // guarded by s_lock
static volatile int s_wake_minutes = WAKE_MINUTES_DEFAULT;
static volatile bool s_manual_off;     // turned off with the button
static volatile bool s_scheduled_off;  // inside the display_off window
static volatile int64_t s_wake_until;  // esp_timer µs; woken during the window
static volatile int s_brightness_override = -1;  // from the button, until day/night flips
static volatile int s_night = -1;                // last day/night phase (-1 unknown)
static volatile bool s_display_dirty = true;
static char s_jump[ACTION_LEN];  // app to show next ("show:<app>")
static volatile bool s_jump_pending;

// --- helpers shared with the apps (app_api.h) ---------------------------------

int cfg_int(const cJSON *cfg, const char *key, int def) {
  const cJSON *v = cJSON_GetObjectItem(cfg, key);
  return cJSON_IsNumber(v) ? v->valueint : def;
}

double cfg_double(const cJSON *cfg, const char *key, double def) {
  const cJSON *v = cJSON_GetObjectItem(cfg, key);
  return cJSON_IsNumber(v) ? v->valuedouble : def;
}

bool cfg_bool(const cJSON *cfg, const char *key, bool def) {
  const cJSON *v = cJSON_GetObjectItem(cfg, key);
  return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

const char *cfg_str(const cJSON *cfg, const char *key, const char *def) {
  const cJSON *v = cJSON_GetObjectItem(cfg, key);
  return cJSON_IsString(v) ? v->valuestring : def;
}

bool cfg_has_number(const cJSON *cfg, const char *key) {
  return cJSON_IsNumber(cJSON_GetObjectItem(cfg, key));
}

rgb_t parse_color(const char *hex, rgb_t def) {
  if (!hex) return def;
  if (hex[0] == '#') hex++;
  if (strlen(hex) != 6) return def;
  char *end;
  unsigned long v = strtoul(hex, &end, 16);
  if (*end) return def;
  return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant).
static int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

time_t parse_iso8601_utc(const char *s) {
  int y, mo, d, h, mi, sec;
  if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
    return 0;
  return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
}

bool time_is_valid(void) { return time(NULL) > 1700000000; }

// Time of day in minutes since midnight. Accepts "HH:MM", or a number of
// hours (older configs). Returns def if missing, -1 if invalid.
static int cfg_time(const cJSON *cfg, const char *key, int def) {
  const cJSON *v = cJSON_GetObjectItem(cfg, key);
  if (!v) return def;
  if (cJSON_IsNumber(v)) {
    return v->valueint >= 0 && v->valueint <= 23 ? v->valueint * 60 : -1;
  }
  int h, m;
  char extra;
  if (cJSON_IsString(v) && sscanf(v->valuestring, "%d:%d%c", &h, &m, &extra) == 2 &&
      h >= 0 && h <= 23 && m >= 0 && m <= 59) {
    return h * 60 + m;
  }
  return -1;
}

// Is minute-of-day `now` within [start, end)? Wraps past midnight when
// start > end; start == end is an empty window.
static bool in_window(int now, int start, int end) {
  return start <= end ? (now >= start && now < end) : (now >= start || now < end);
}

static int minute_of_day(void) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  return tm.tm_hour * 60 + tm.tm_min;
}

// --- config -------------------------------------------------------------------

static void *psram_malloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  return p ? p : malloc(size);
}

static const app_t *find_app(const char *name) {
  for (size_t i = 0; i < APP_COUNT; i++) {
    if (name && strcmp(APPS[i]->name, name) == 0) return APPS[i];
  }
  return NULL;
}

// Button actions: none, next, display (on/off), brightness (step through
// BRIGHTNESS_STEPS) or show:<app> (jump to that app in the rotation).
static bool valid_action(const char *a) {
  return strlen(a) < ACTION_LEN &&
         (!strcmp(a, "none") || !strcmp(a, "next") || !strcmp(a, "display") ||
          !strcmp(a, "brightness") || (!strncmp(a, "show:", 5) && find_app(a + 5)));
}

static esp_err_t validate(const cJSON *root, const char **error) {
  if (!cJSON_IsObject(root)) return *error = "config must be a JSON object", ESP_ERR_INVALID_ARG;
  const cJSON *rotation = cJSON_GetObjectItem(root, "rotation");
  if (!cJSON_IsArray(rotation)) return *error = "rotation must be an array", ESP_ERR_INVALID_ARG;
  const cJSON *item;
  cJSON_ArrayForEach(item, rotation) {
    if (!find_app(cfg_str(item, "app", NULL)))
      return *error = "rotation item with unknown or missing app", ESP_ERR_INVALID_ARG;
    int dwell = cfg_int(item, "dwell", 10);
    if (dwell < 1 || dwell > 3600) return *error = "dwell must be 1-3600", ESP_ERR_INVALID_ARG;
    const cJSON *c = cJSON_GetObjectItem(item, "config");
    if (c && !cJSON_IsObject(c)) return *error = "item config must be an object", ESP_ERR_INVALID_ARG;
  }
  const cJSON *tz = cJSON_GetObjectItem(root, "timezone");
  if (tz && !cJSON_IsString(tz)) return *error = "timezone must be a string", ESP_ERR_INVALID_ARG;

  const cJSON *b = cJSON_GetObjectItem(root, "brightness");
  if (b) {
    if (!cJSON_IsObject(b)) return *error = "brightness must be an object", ESP_ERR_INVALID_ARG;
    int day = cfg_int(b, "day", 60), night = cfg_int(b, "night", 10);
    if (day < 0 || day > 100 || night < 0 || night > 100)
      return *error = "brightness must be 0-100", ESP_ERR_INVALID_ARG;
    if (cfg_time(b, "night_start", 0) < 0 || cfg_time(b, "night_end", 0) < 0)
      return *error = "night times must be HH:MM", ESP_ERR_INVALID_ARG;
  }

  const cJSON *off = cJSON_GetObjectItem(root, "display_off");
  if (off) {
    if (!cJSON_IsObject(off)) return *error = "display_off must be an object", ESP_ERR_INVALID_ARG;
    if (cfg_time(off, "start", 0) < 0 || cfg_time(off, "end", 0) < 0)
      return *error = "display off times must be HH:MM", ESP_ERR_INVALID_ARG;
    int wake = cfg_int(off, "wake_minutes", WAKE_MINUTES_DEFAULT);
    if (wake < 1 || wake > 240) return *error = "wake_minutes must be 1-240", ESP_ERR_INVALID_ARG;
  }

  const cJSON *button = cJSON_GetObjectItem(root, "button");
  if (button) {
    if (!cJSON_IsObject(button)) return *error = "button must be an object", ESP_ERR_INVALID_ARG;
    static const char *const gestures[] = {"tap", "double_tap", "hold"};
    for (size_t i = 0; i < 3; i++) {
      if (!valid_action(cfg_str(button, gestures[i], "none")))
        return *error = "unknown button action", ESP_ERR_INVALID_ARG;
    }
  }
  return ESP_OK;
}

static char *load_stored_config(void) {
  nvs_handle_t h;
  char *json = NULL;
  size_t len = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
      json = psram_malloc(len + 1);
      if (json && nvs_get_blob(h, NVS_KEY, json, &len) == ESP_OK) {
        json[len] = '\0';
      } else {
        free(json);
        json = NULL;
      }
    }
    nvs_close(h);
  }
  if (json) {
    const char *error;
    cJSON *root = cJSON_Parse(json);
    bool ok = validate(root, &error) == ESP_OK;
    cJSON_Delete(root);
    if (ok) return json;
    ESP_LOGW(TAG, "stored config invalid (%s), using defaults", error);
    free(json);
  }
  return strdup(DEFAULT_CONFIG);
}

static button_t parse_button(const cJSON *root) {
  const cJSON *button = cJSON_GetObjectItem(root, "button");
  button_t bt = {.enabled = cfg_bool(button, "enabled", BUTTON_DEFAULT_ENABLED)};
  snprintf(bt.tap, ACTION_LEN, "%s", cfg_str(button, "tap", "next"));
  snprintf(bt.double_tap, ACTION_LEN, "%s", cfg_str(button, "double_tap", "none"));
  snprintf(bt.hold, ACTION_LEN, "%s", cfg_str(button, "hold", "display"));
  return bt;
}

void apps_init(void) {
  if (s_lock) return;
  // Big API responses (departures, Spotify) parse into thousands of small
  // cJSON nodes; keep them out of the scarce internal RAM.
  cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
  cJSON_InitHooks(&hooks);
  s_config_json = load_stored_config();
  // The main task polls the button settings before the apps task has run.
  cJSON *root = cJSON_Parse(s_config_json);
  s_button = parse_button(root);
  cJSON_Delete(root);
  s_lock = xSemaphoreCreateMutex();
}

char *apps_get_config_json(void) {
  if (!s_lock) return strdup(DEFAULT_CONFIG);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  char *copy = strdup(s_config_json ? s_config_json : DEFAULT_CONFIG);
  xSemaphoreGive(s_lock);
  return copy;
}

esp_err_t apps_set_config_json(const char *json, const char **error) {
  if (!s_lock) {
    *error = "not initialised";
    return ESP_ERR_INVALID_STATE;
  }
  cJSON *root = cJSON_Parse(json);
  if (!root) {
    *error = "invalid JSON";
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = validate(root, error);
  if (err != ESP_OK) {
    cJSON_Delete(root);
    return err;
  }
  char *compact = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!compact) return ESP_ERR_NO_MEM;

  nvs_handle_t h;
  err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err == ESP_OK) {
    err = nvs_set_blob(h, NVS_KEY, compact, strlen(compact));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
  }
  if (err != ESP_OK) {
    *error = "could not save to flash";
    free(compact);
    return err;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  free(s_config_json);
  s_config_json = compact;
  xSemaphoreGive(s_lock);
  s_config_changed = true;  // the task picks it up at the next frame
  ESP_LOGI(TAG, "config updated");
  return ESP_OK;
}

static cJSON *take_config(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  cJSON *root = cJSON_Parse(s_config_json);
  s_config_changed = false;
  xSemaphoreGive(s_lock);
  const char *tz = cfg_str(root, "timezone", "CET-1CEST,M3.5.0,M10.5.0/3");
  setenv("TZ", tz, 1);
  tzset();

  button_t bt = parse_button(root);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_button = bt;
  xSemaphoreGive(s_lock);
  s_wake_minutes = cfg_int(cJSON_GetObjectItem(root, "display_off"), "wake_minutes",
                           WAKE_MINUTES_DEFAULT);
  s_brightness_override = -1;
  s_display_dirty = true;
  return root;
}

// --- runtime ------------------------------------------------------------------

static bool display_on(void) {
  return !s_manual_off && (!s_scheduled_off || esp_timer_get_time() < s_wake_until);
}

// Apply the display_off window and the day/night brightness (or a brightness
// chosen with the button). Cheap; called about once a second.
static void update_display(const cJSON *cfg) {
  bool force = s_display_dirty;
  s_display_dirty = false;
  bool clock = time_is_valid();
  int now = clock ? minute_of_day() : 0;

  const cJSON *off = cJSON_GetObjectItem(cfg, "display_off");
  bool scheduled = clock && cfg_bool(off, "enabled", false) &&
                   in_window(now, cfg_time(off, "start", 0), cfg_time(off, "end", 6 * 60));
  // Turning the display off by hand lasts until the next scheduled off
  // period has ended, so it comes back on in the morning.
  if (s_scheduled_off && !scheduled) s_manual_off = false;
  s_scheduled_off = scheduled;

  int target = 0;
  if (display_on()) {
    const cJSON *b = cJSON_GetObjectItem(cfg, "brightness");
    int night = clock && in_window(now, cfg_time(b, "night_start", 22 * 60),
                                   cfg_time(b, "night_end", 7 * 60));
    if (night != s_night) s_brightness_override = -1;  // day/night flipped
    s_night = night;
    target = s_brightness_override >= 0 ? s_brightness_override
             : night                    ? cfg_int(b, "night", 10)
                                        : cfg_int(b, "day", 60);
  }
  if (target < DISPLAY_MIN_BRIGHTNESS) target = DISPLAY_MIN_BRIGHTNESS;
  if (target > DISPLAY_MAX_BRIGHTNESS) target = DISPLAY_MAX_BRIGHTNESS;
  if (force || target != s_brightness) {
    display_set_brightness((uint8_t)target);
    s_brightness = target;
  }
}

// Should the app on screen give way? Consumes skip requests.
static bool interrupted(void) {
  if (s_skip) {
    s_skip = false;
    return true;
  }
  return s_config_changed || s_jump_pending || !display_on() ||
         uxQueueMessagesWaiting(s_notifications) > 0;
}

typedef uint32_t (*frame_fn)(const void *ctx, uint32_t t_ms);

// Draw frames until min_ms (or longer if the content needs it) has passed.
static void run_frames(frame_fn draw, const void *ctx, uint32_t min_ms,
                       bool interruptible, const cJSON *cfg) {
  int64_t start = esp_timer_get_time();
  uint32_t need = 0;
  int64_t last_update = start;
  for (;;) {
    uint32_t t = (uint32_t)((esp_timer_get_time() - start) / 1000);
    uint32_t until = min_ms > need ? min_ms : need;
    if (until > MAX_SHOW_MS) until = MAX_SHOW_MS;
    if (t >= until) break;
    if (interruptible && interrupted()) break;
    canvas_clear(RGB(0, 0, 0));
    need = draw(ctx, t);
    canvas_present();
    if (s_display_dirty || esp_timer_get_time() - last_update > 1000000LL) {
      update_display(cfg);
      last_update = esp_timer_get_time();
    }
    vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
  }
}

typedef struct {
  const app_t *app;
  const cJSON *cfg;
} app_ctx_t;

static uint32_t draw_app(const void *ctx, uint32_t t_ms) {
  const app_ctx_t *c = ctx;
  return c->app->draw(c->cfg, t_ms);
}

static uint32_t draw_notification(const void *ctx, uint32_t t_ms) {
  const notification_t *n = ctx;
  return message_draw(n->title, n->text, parse_color(n->color, RGB(255, 255, 255)),
                      t_ms);
}

// Index of the first enabled rotation item running `app`, or -1.
static int find_rotation_item(const cJSON *rotation, const char *app) {
  int i = 0;
  const cJSON *item;
  cJSON_ArrayForEach(item, rotation) {
    if (cfg_bool(item, "enabled", true) && !strcmp(cfg_str(item, "app", ""), app)) return i;
    i++;
  }
  return -1;
}

static void apps_task(void *arg) {
  // The WebP renderer stays paused while the apps own the display.
  gfx_stop();
  vTaskDelay(pdMS_TO_TICKS(200));
  isAnimating = 0;

  cJSON *cfg = take_config();
  int index = 0, skipped = 0;

  for (;;) {
    if (s_config_changed) {
      cJSON_Delete(cfg);
      cfg = take_config();
    }
    update_display(cfg);
    if (!display_on()) {
      // Off: no drawing and no data fetching until the display comes back.
      if (s_current[0]) {
        canvas_clear(RGB(0, 0, 0));
        canvas_present();
        s_current = "";
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    notification_t n;
    if (xQueueReceive(s_notifications, &n, 0) == pdTRUE) {
      s_current = "notification";
      run_frames(draw_notification, &n, n.secs * 1000, false, cfg);
      continue;
    }

    const cJSON *rotation = cJSON_GetObjectItem(cfg, "rotation");
    int count = cJSON_GetArraySize(rotation);
    if (s_jump_pending) {
      int i = find_rotation_item(rotation, s_jump);
      if (i >= 0) {
        index = i;
        skipped = 0;
      }
      s_jump_pending = false;
    }
    if (count == 0 || skipped >= count) {
      // Nothing to show right now (all disabled or all skipping): show the
      // clock rather than a frozen frame.
      app_ctx_t c = {&app_clock, NULL};
      s_current = "clock";
      run_frames(draw_app, &c, 5000, true, cfg);
      skipped = 0;
      continue;
    }

    const cJSON *item = cJSON_GetArrayItem(rotation, index++ % count);
    const app_t *app = find_app(cfg_str(item, "app", NULL));
    const cJSON *app_cfg = cJSON_GetObjectItem(item, "config");
    if (!app || !cfg_bool(item, "enabled", true) || !app->prepare(app_cfg)) {
      skipped++;
      continue;
    }
    skipped = 0;
    s_current = app->name;
    app_ctx_t c = {app, app_cfg};
    run_frames(draw_app, &c, cfg_int(item, "dwell", 10) * 1000, true, cfg);
  }
}

// --- button -------------------------------------------------------------------

static void wake_display(void) {
  s_manual_off = false;
  if (s_scheduled_off) s_wake_until = esp_timer_get_time() + s_wake_minutes * 60000000LL;
  ESP_LOGI(TAG, "display on");
}

static void turn_display_off(void) {
  if (s_scheduled_off) {
    s_wake_until = 0;  // back to the scheduled off period
  } else {
    s_manual_off = true;
  }
  ESP_LOGI(TAG, "display off");
}

static void step_brightness(void) {
  int next = BRIGHTNESS_STEPS[0];
  for (size_t i = 0; i < BRIGHTNESS_STEP_COUNT; i++) {
    if (BRIGHTNESS_STEPS[i] > s_brightness) {
      next = BRIGHTNESS_STEPS[i];
      break;
    }
  }
  s_brightness_override = next;
  ESP_LOGI(TAG, "brightness %d%% (until the next day/night switch)", next);
}

// --- public API -----------------------------------------------------------------

bool apps_local_mode(const char *image_url) {
  return image_url == NULL || image_url[0] == '\0' || strcmp(image_url, "local") == 0;
}

void apps_start(void) {
  if (s_task) return;
  apps_init();
  s_notifications = xQueueCreate(8, sizeof(notification_t));
  xTaskCreatePinnedToCore(apps_task, "apps", TASK_STACK, NULL, 2, &s_task, 1);
  ESP_LOGI(TAG, "local apps started");
}

esp_err_t apps_notify(const char *title, const char *text, const char *color,
                      int secs) {
  if (!s_notifications) return ESP_ERR_INVALID_STATE;
  notification_t n = {.secs = secs > 0 ? secs : 15};
  if (n.secs > 600) n.secs = 600;
  snprintf(n.title, sizeof(n.title), "%s", title ? title : "");
  snprintf(n.text, sizeof(n.text), "%s", text ? text : "");
  snprintf(n.color, sizeof(n.color), "%s", color ? color : "");
  return xQueueSend(s_notifications, &n, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void apps_next(void) { s_skip = true; }

bool apps_button_enabled(void) { return s_lock && s_button.enabled; }

bool apps_button_double_tap(void) {
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool used = strcmp(s_button.double_tap, "none") != 0;
  xSemaphoreGive(s_lock);
  return used;
}

void apps_button(const char *gesture) {
  if (!s_lock || !s_task) return;
  char action[ACTION_LEN];
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const char *a = !strcmp(gesture, "tap")          ? s_button.tap
                  : !strcmp(gesture, "double_tap") ? s_button.double_tap
                                                   : s_button.hold;
  snprintf(action, sizeof(action), "%s", a);
  xSemaphoreGive(s_lock);
  ESP_LOGI(TAG, "button %s -> %s", gesture, action);
  if (!strcmp(action, "none")) return;

  // While the display is off, any assigned gesture just turns it back on.
  if (!display_on()) {
    wake_display();
  } else if (!strcmp(action, "next")) {
    s_skip = true;
  } else if (!strcmp(action, "display")) {
    turn_display_off();
  } else if (!strcmp(action, "brightness")) {
    step_brightness();
  } else if (!strncmp(action, "show:", 5)) {
    apps_show(action + 5);
  }
  s_display_dirty = true;
}

void apps_set_display(bool on) {
  if (on == display_on()) return;
  if (on) {
    wake_display();
  } else {
    turn_display_off();
  }
  s_display_dirty = true;
}

void apps_set_brightness(int pct) {
  s_brightness_override = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  s_display_dirty = true;
}

int apps_brightness(void) { return s_brightness < 0 ? 0 : s_brightness; }

esp_err_t apps_show(const char *app) {
  if (!s_lock || !find_app(app) || strlen(app) >= ACTION_LEN) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  cJSON *root = cJSON_Parse(s_config_json);
  xSemaphoreGive(s_lock);
  int i = find_rotation_item(cJSON_GetObjectItem(root, "rotation"), app);
  cJSON_Delete(root);
  if (i < 0) return ESP_ERR_INVALID_ARG;
  snprintf(s_jump, sizeof(s_jump), "%s", app);
  s_jump_pending = true;
  return ESP_OK;
}

const char *apps_display_state(void) {
  if (display_on()) return s_scheduled_off ? "on (woken)" : "on";
  return s_manual_off ? "off" : "off (schedule)";
}

const char *apps_current(void) { return s_current; }

char *apps_list_json(void) {
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < APP_COUNT; i++)
    cJSON_AddItemToArray(arr, cJSON_CreateString(APPS[i]->name));
  char *out = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  return out;
}
