/*
 * The gateway loop (python/zivyobraz_ble_gateway/service.py): ask zivyobraz.eu for each tag on its
 * schedule, push changed pictures over BLE.
 *
 * The tags never call us; they only advertise. For every registered tag whose next_poll has
 * come, the gateway task POSTs the FW 3.0 request. If the answer's Timestamp differs from the
 * one of the picture on the tag (or a refresh was asked for), the body is saved to LittleFS
 * then decoded into the packed payload and sent (one transfer at a time, the scanner pauses
 * meanwhile). The stored Timestamp changes only after a successful
 * transfer; a failed one is retried by polling again after 1, 2, 4 ... 30 min.
 *
 * Unlike the Python gateway there is no queue of pending pictures: without PSRAM there is
 * room for one payload at a time, so the poll waits for its transfer.
 */
#include "gateway.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "eth.h"
#include "guard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "status_led.h"
#include "update.h"
#include "zivyobraz.h"

static const char *TAG = "gateway";

#define TAGS_FILE "/data/tags.json"
#define TAGS_TMP "/data/tags.tmp"
#define PREVIEW_DIR "/data/p"
#define WORK_IMG "/data/work.img"  /* packed payload when RAM can't hold the pages */
#define ERROR_RETRY_SECONDS 300    /* server unreachable / bad answer */
#define RETRY_SECONDS 60           /* failed transfer: 1, 2, 4, ... min */
#define MAX_RETRY_SECONDS 1800
#define SEEN_FORGET_SECONDS 600
#define SAVE_EVERY_SECONDS 60

static SemaphoreHandle_t s_lock, s_log_lock;
static gw_tag_t s_tags[GW_MAX_TAGS];
static int s_ntags;
static gw_seen_t s_seen[GW_MAX_SEEN];
static int s_nseen;
static char s_busy[18];
static bool s_dirty;
static TaskHandle_t s_task;
static gw_settings_t s_settings;
static uint32_t s_preview_counter;

static char s_log[GW_LOG_LINES][GW_LOG_LEN];
static int s_log_head, s_log_count;

/* ---- time and log --------------------------------------------------------------------- */

int64_t gw_uptime(void)
{
    return esp_timer_get_time() / 1000000 + 1; /* never 0: 0 means "never" */
}

bool gw_clock_set(void)
{
    return time(NULL) > 1700000000;
}

void gw_when(int64_t t, char *out, size_t size)
{
    if (t <= 0) {
        snprintf(out, size, "never");
        return;
    }
    int64_t age = gw_uptime() - t;
    if (!gw_clock_set()) {
        if (age < 90) {
            snprintf(out, size, "%d s ago", (int)age);
        } else if (age < 5400) {
            snprintf(out, size, "%d min ago", (int)((age + 30) / 60));
        } else {
            snprintf(out, size, "%.1f h ago", age / 3600.0);
        }
        return;
    }
    time_t now = time(NULL), then = now - (time_t)age;
    struct tm a, b;
    localtime_r(&now, &a);
    localtime_r(&then, &b);
    strftime(out, size, a.tm_yday == b.tm_yday && a.tm_year == b.tm_year ? "%H:%M:%S" : "%d.%m. %H:%M", &b);
}

void gw_log(const char *fmt, ...)
{
    char text[GW_LOG_LEN - 9];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", text);

    char stamp[16];
    if (gw_clock_set()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    } else { /* uptime until NTP answers */
        int64_t s = gw_uptime();
        snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", (int)(s / 3600 % 100), (int)(s / 60 % 60), (int)(s % 60));
    }
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    snprintf(s_log[s_log_head], GW_LOG_LEN, "%.8s %s", stamp, text);
    s_log_head = (s_log_head + 1) % GW_LOG_LINES;
    if (s_log_count < GW_LOG_LINES) {
        s_log_count++;
    }
    xSemaphoreGive(s_log_lock);
}

void gw_log_each(void (*fn)(void *ctx, const char *line), void *ctx)
{
    /* copy first: fn sends to the network and mustn't hold the lock */
    char(*lines)[GW_LOG_LEN] = malloc(sizeof(s_log));
    if (!lines) {
        return;
    }
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    int n = s_log_count;
    for (int i = 0; i < n; i++) {
        memcpy(lines[i], s_log[(s_log_head - 1 - i + GW_LOG_LINES) % GW_LOG_LINES], GW_LOG_LEN);
    }
    xSemaphoreGive(s_log_lock);
    for (int i = 0; i < n; i++) {
        fn(ctx, lines[i]);
    }
    free(lines);
}

/* ---- registry ------------------------------------------------------------------------- */

void gw_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void gw_unlock(void) { xSemaphoreGive(s_lock); }

static gw_tag_t *find_tag(const char *mac)
{
    for (int i = 0; i < s_ntags; i++) {
        if (strcasecmp(s_tags[i].mac, mac) == 0) {
            return &s_tags[i];
        }
    }
    return NULL;
}

static gw_seen_t *find_seen(const char *mac)
{
    for (int i = 0; i < s_nseen; i++) {
        if (strcasecmp(s_seen[i].adv.mac, mac) == 0) {
            return &s_seen[i];
        }
    }
    return NULL;
}

void gw_preview_path(const char *mac, char *out, size_t size)
{
    char name[13];
    int j = 0;
    for (const char *p = mac; *p && j < 12; p++) {
        if (*p != ':') {
            name[j++] = (char)toupper((unsigned char)*p);
        }
    }
    name[j] = 0;
    snprintf(out, size, PREVIEW_DIR "/%s.z", name);
}

static void upper(char *s)
{
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static cJSON *tags_json(void)
{
    cJSON *root = cJSON_CreateObject();
    for (int i = 0; i < s_ntags; i++) {
        const gw_tag_t *t = &s_tags[i];
        cJSON *e = cJSON_AddObjectToObject(root, t->mac);
        cJSON_AddStringToObject(e, "api_key", t->api_key);
        cJSON_AddNumberToObject(e, "added", (double)t->added);
        cJSON_AddNumberToObject(e, "sdk_type", t->sdk_type);
        cJSON_AddNumberToObject(e, "raw_type", t->raw_type);
        cJSON_AddNumberToObject(e, "sw", t->sw);
        cJSON_AddNumberToObject(e, "hw", t->hw);
        cJSON_AddNumberToObject(e, "addr_type", t->addr_type);
        if (t->timestamp[0]) {
            cJSON_AddStringToObject(e, "timestamp", t->timestamp);
        } else {
            cJSON_AddNullToObject(e, "timestamp");
        }
        cJSON_AddBoolToObject(e, "rotate", t->rotate);
    }
    return root;
}

/* Under gw_lock. Write + rename: a crash can't leave half a file. */
static void save(void)
{
    cJSON *root = tags_json();
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        return;
    }
    FILE *f = fopen(TAGS_TMP, "w");
    bool ok = f && fputs(text, f) >= 0;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    free(text);
    if (ok) {
        remove(TAGS_FILE);
        ok = rename(TAGS_TMP, TAGS_FILE) == 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "can't save " TAGS_FILE);
    }
    s_dirty = false;
}

static int num(const cJSON *e, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

/* Fill a tag from a tags.json entry (also the Python gateway's). false if unusable. */
static bool tag_from_json(gw_tag_t *t, const char *mac, const cJSON *e)
{
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(e, "api_key");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(e, "timestamp");
    const cJSON *added = cJSON_GetObjectItemCaseSensitive(e, "added");
    int sdk = num(e, "sdk_type", -1);
    if (!guard_valid_mac(mac) || !cJSON_IsString(key) || !guard_valid_key(key->valuestring) || !ps_model(sdk)) {
        return false;
    }
    memset(t, 0, sizeof(*t));
    snprintf(t->mac, sizeof(t->mac), "%s", mac);
    upper(t->mac);
    snprintf(t->api_key, sizeof(t->api_key), "%s", key->valuestring);
    t->sdk_type = (uint16_t)sdk;
    t->raw_type = (uint16_t)num(e, "raw_type", PS_RAW_DATA_FLAG | sdk);
    t->sw = (uint8_t)num(e, "sw", 0);
    t->hw = (uint8_t)num(e, "hw", 0);
    /* the Python file has no address type: a static random address starts with 11 binary */
    unsigned first = 0;
    sscanf(mac, "%2x", &first);
    t->addr_type = (uint8_t)num(e, "addr_type", (first & 0xC0) == 0xC0 ? 1 : 0);
    if (cJSON_IsString(ts)) {
        snprintf(t->timestamp, sizeof(t->timestamp), "%s", ts->valuestring);
    }
    t->rotate = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "rotate"));
    t->added = cJSON_IsNumber(added) ? (int64_t)added->valuedouble : 0;
    return true;
}

static void preview_check(gw_tag_t *t)
{
    char path[48];
    struct stat st;
    gw_preview_path(t->mac, path, sizeof(path));
    if (stat(path, &st) == 0) {
        t->preview_version = ++s_preview_counter;
    }
}

static void load(void)
{
    FILE *f = fopen(TAGS_FILE, "r");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    if (!text) {
        fclose(f);
        return;
    }
    text[fread(text, 1, (size_t)n, f)] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(text);
    free(text);
    const cJSON *e;
    cJSON_ArrayForEach(e, root)
    {
        if (s_ntags < GW_MAX_TAGS && tag_from_json(&s_tags[s_ntags], e->string, e)) {
            preview_check(&s_tags[s_ntags]);
            s_ntags++;
        }
    }
    cJSON_Delete(root);
}

static void remove_previews(const char *mac)
{
    char path[48];
    gw_preview_path(mac, path, sizeof(path));
    remove(path);
    /* firmware up to 0.1.2 also kept PNG bodies, as "<name>.png" next to it */
    size_t n = strlen(path);
    if (n + 2 < sizeof(path)) {
        strcpy(path + n - 1, "png");
        remove(path);
    }
}

/* ---- settings (NVS) ------------------------------------------------------------------- */

static void load_settings(void)
{
    s_settings = (gw_settings_t){CONFIG_ZO_MIN_INTERVAL, CONFIG_ZO_MAX_INTERVAL};
    nvs_handle_t h;
    if (nvs_open("gw", NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "min", &v) == ESP_OK) {
            s_settings.min_interval = v;
        }
        if (nvs_get_i32(h, "max", &v) == ESP_OK) {
            s_settings.max_interval = v;
        }
        nvs_close(h);
    }
}

gw_settings_t gw_get_settings(void)
{
    return s_settings;
}

void gw_set_settings(gw_settings_t s)
{
    if (s.min_interval < 10) {
        s.min_interval = 10;
    }
    if (s.max_interval < s.min_interval) {
        s.max_interval = s.min_interval;
    }
    s_settings = s;
    nvs_handle_t h;
    if (nvs_open("gw", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "min", s.min_interval);
        nvs_set_i32(h, "max", s.max_interval);
        nvs_commit(h);
        nvs_close(h);
    }
    gw_log("settings: ask every %d .. %d s", s.min_interval, s.max_interval);
}

/* ---- web page actions ----------------------------------------------------------------- */

gw_tag_t *gw_tags_copy(int *n)
{
    gw_lock();
    gw_tag_t *copy = malloc(sizeof(gw_tag_t) * (s_ntags ? s_ntags : 1));
    *n = copy ? s_ntags : 0;
    if (copy) {
        memcpy(copy, s_tags, sizeof(gw_tag_t) * s_ntags);
    }
    gw_unlock();
    return copy;
}

gw_seen_t *gw_seen_copy(int *n)
{
    gw_lock();
    gw_seen_t *copy = malloc(sizeof(gw_seen_t) * (s_nseen ? s_nseen : 1));
    *n = copy ? s_nseen : 0;
    if (copy) {
        memcpy(copy, s_seen, sizeof(gw_seen_t) * s_nseen);
    }
    gw_unlock();
    return copy;
}

void gw_busy(char out[18])
{
    gw_lock();
    memcpy(out, s_busy, 18);
    gw_unlock();
}

void gw_wake(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool gw_add(const char *mac, char *err, size_t size)
{
    gw_lock();
    gw_seen_t *seen = find_seen(mac);
    bool ok = false;
    if (find_tag(mac)) {
        snprintf(err, size, "%s is already added", mac);
    } else if (!seen) {
        snprintf(err, size, "%s is not in range", mac);
    } else if (!ps_model(seen->adv.adv.sdk_type)) {
        snprintf(err, size, "%s: unknown display type %d", mac, seen->adv.adv.sdk_type);
    } else if (s_ntags >= GW_MAX_TAGS) {
        snprintf(err, size, "the tag list is full (%d tags)", GW_MAX_TAGS);
    } else {
        gw_tag_t *t = &s_tags[s_ntags++];
        const psb_advert_t *a = &seen->adv;
        *t = (gw_tag_t){
            .addr_type = a->addr_type, .sdk_type = a->adv.sdk_type, .raw_type = a->adv.raw_type,
            .sw = a->adv.sw, .hw = a->adv.hw, .battery_dv = a->adv.battery_dv, .rssi = a->rssi,
            .last_seen = seen->seen_at, .added = gw_clock_set() ? (int64_t)time(NULL) : 0,
        };
        snprintf(t->mac, sizeof(t->mac), "%s", a->mac);
        zo_api_key(esp_random(), t->api_key);
        save();
        gw_log("added %s (%s), zivyobraz key %s", t->mac, ps_model(t->sdk_type)->desc, t->api_key);
        ok = true;
    }
    gw_unlock();
    if (ok) {
        gw_wake();
    }
    return ok;
}

bool gw_remove(const char *mac)
{
    gw_lock();
    gw_tag_t *t = find_tag(mac);
    if (t) {
        char m[18];
        memcpy(m, t->mac, sizeof(m));
        *t = s_tags[--s_ntags];
        save();
        remove_previews(m);
        gw_log("removed %s", m);
    }
    gw_unlock();
    return t != NULL;
}

bool gw_refresh(const char *mac)
{
    gw_lock();
    gw_tag_t *t = find_tag(mac);
    if (t) {
        t->forced = true;
        t->next_poll = 0;
        gw_log("%s: refresh requested", t->mac);
    }
    gw_unlock();
    if (t) {
        gw_wake();
    }
    return t != NULL;
}

char *gw_export(void)
{
    gw_lock();
    cJSON *root = tags_json();
    gw_unlock();
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

bool gw_import(const char *json, int *added, char *err, size_t size)
{
    *added = 0;
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, size, "not a tags.json file");
        return false;
    }
    int skipped = 0;
    gw_lock();
    const cJSON *e;
    cJSON_ArrayForEach(e, root)
    {
        gw_tag_t t;
        if (!tag_from_json(&t, e->string, e)) {
            skipped++;
            continue;
        }
        gw_tag_t *old = find_tag(t.mac);
        if (old) { /* keep what we know at runtime, take the identity from the file */
            memcpy(old->api_key, t.api_key, sizeof(t.api_key));
            memcpy(old->timestamp, t.timestamp, sizeof(t.timestamp));
            old->next_poll = 0;
        } else if (s_ntags < GW_MAX_TAGS) {
            s_tags[s_ntags++] = t;
        } else {
            skipped++;
            continue;
        }
        (*added)++;
    }
    if (*added) {
        save();
    }
    gw_unlock();
    cJSON_Delete(root);
    gw_log("imported %d tag(s)%s", *added, skipped ? ", some entries skipped" : "");
    gw_wake();
    return true;
}

/* ---- adverts (NimBLE host task) ------------------------------------------------------- */

static void on_advert(const psb_advert_t *a)
{
    int64_t now = gw_uptime();
    gw_lock();
    gw_seen_t *s = find_seen(a->mac);
    gw_tag_t *t = find_tag(a->mac);
    if (!s) {
        if (s_nseen < GW_MAX_SEEN) {
            s = &s_seen[s_nseen++];
        } else { /* full: replace the one heard longest ago */
            s = &s_seen[0];
            for (int i = 1; i < s_nseen; i++) {
                if (s_seen[i].seen_at < s->seen_at) {
                    s = &s_seen[i];
                }
            }
        }
        const ps_model_t *m = ps_model(a->adv.sdk_type);
        gw_log("in range: %s %s, %d.%d V (%s)", a->mac, m ? m->desc : "unknown type", a->adv.battery_dv / 10,
               a->adv.battery_dv % 10, t ? "registered" : "not registered");
    }
    s->adv = *a;
    s->seen_at = now;
    if (t) {
        if (t->sdk_type != a->adv.sdk_type || t->raw_type != a->adv.raw_type || t->sw != a->adv.sw ||
            t->hw != a->adv.hw || t->addr_type != a->addr_type) {
            s_dirty = true;
        }
        t->sdk_type = a->adv.sdk_type, t->raw_type = a->adv.raw_type, t->sw = a->adv.sw, t->hw = a->adv.hw;
        t->addr_type = a->addr_type, t->battery_dv = a->adv.battery_dv, t->rssi = a->rssi, t->last_seen = now;
    }
    gw_unlock();
}

static void forget_old(void)
{
    int64_t limit = gw_uptime() - SEEN_FORGET_SECONDS;
    gw_lock();
    for (int i = 0; i < s_nseen;) {
        if (s_seen[i].seen_at < limit) {
            s_seen[i] = s_seen[--s_nseen];
        } else {
            i++;
        }
    }
    gw_unlock();
}

/* ---- one poll (service.py `poll` + `transfer`) ---------------------------------------- */

typedef struct {
    gw_tag_t job; /* copy of the tag when the poll started */
    bool forced, changed, want;
    ps_image_t *img;
    zo_decoder_t *dec;
    FILE *preview;
    size_t preview_len;
    bool bad;
    char bad_reason[72];
    char tmp[48];
} poll_t;

static bool want_body(void *ctx, const zo_answer_t *a)
{
    poll_t *p = ctx;
    p->changed = !a->timestamp[0] || strcmp(a->timestamp, p->job.timestamp) != 0;
    if (!(p->changed || p->forced)) {
        return false;
    }
    p->want = true;
    snprintf(p->tmp, sizeof(p->tmp), PREVIEW_DIR "/new.tmp");
    p->preview = fopen(p->tmp, "wb");
    if (!p->preview) {
        p->bad = true;
        snprintf(p->bad_reason, sizeof(p->bad_reason), "can't write %s", p->tmp);
        return false;
    }
    return true;
}

static bool on_body(void *ctx, const uint8_t *data, size_t len)
{
    poll_t *p = ctx;
    if (!p->preview || fwrite(data, 1, len, p->preview) != len) {
        p->bad = true;
        snprintf(p->bad_reason, sizeof(p->bad_reason), "preview write failed");
        return false;
    }
    p->preview_len += len;
    return true;
}

/* HTTP is finished (TLS heap is back). Decode the saved body into a packed payload. */
static void decode_body(poll_t *p, bool rotate)
{
    const ps_model_t *m = ps_model(p->job.sdk_type);
    FILE *f = fopen(p->tmp, "rb");
    p->img = ps_image_new(p->job.sdk_type, rotate);
    if (!p->img) {
        p->img = ps_image_new_file(p->job.sdk_type, rotate, WORK_IMG);
        if (p->img && m) {
            gw_log("%s: %ux%u packed image on flash (RAM free %u B)", p->job.mac, m->width, m->height,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        }
    }
    p->dec = p->img && f && m ? zo_decoder_new(p->job.sdk_type, m->width, m->height, zo_sink_image, p->img) : NULL;
    if (!p->img || !p->dec || !f) {
        p->bad = true;
        snprintf(p->bad_reason, sizeof(p->bad_reason), "not enough memory for %ux%u (free %u B)", m ? m->width : 0,
                 m ? m->height : 0, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        uint8_t buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (!zo_decoder_feed(p->dec, buf, n)) {
                p->bad = true;
                snprintf(p->bad_reason, sizeof(p->bad_reason), "%s", zo_decoder_error(p->dec));
                break;
            }
        }
        if (!p->bad && !zo_decoder_finish(p->dec)) {
            p->bad = true;
            snprintf(p->bad_reason, sizeof(p->bad_reason), "%s", zo_decoder_error(p->dec));
        }
    }
    if (f) {
        fclose(f);
    }
}

/* Back under the lock: the tag may have been removed meanwhile. */
static gw_tag_t *relock(const char *mac)
{
    gw_lock();
    gw_tag_t *t = find_tag(mac);
    if (!t) {
        gw_unlock();
    }
    return t;
}

static void transfer(poll_t *p, const char *timestamp, const char *reason)
{
    const char *mac = p->job.mac;
    gw_lock();
    memcpy(s_busy, mac, sizeof(s_busy));
    gw_unlock();
    status_led_set(STATUS_BUSY);
    gw_log("%s: sending image (%s)", mac, reason);
    int64_t started = gw_uptime();

    psb_transfer_t t = {
        .mac = mac, .addr_type = p->job.addr_type, .sdk_type = p->job.sdk_type, .sw = p->job.sw,
        .framed = !(p->job.raw_type & PS_RAW_DATA_FLAG), .image = p->img,
    };
    char err[80];
    int rc = psb_send(&t, err, sizeof(err));
    status_led_set(STATUS_OK);

    gw_lock();
    s_busy[0] = 0;
    gw_tag_t *tag = find_tag(mac);
    if (tag) {
        int64_t now = gw_uptime();
        tag->last_push = now;
        if (rc == 0) {
            snprintf(tag->timestamp, sizeof(tag->timestamp), "%s", timestamp);
            tag->push_failures = 0;
            snprintf(tag->push_result, sizeof(tag->push_result), "ok");
            save();
            gw_log("%s: image on the tag, %d s", mac, (int)(now - started));
        } else {
            tag->push_failures++;
            int shift = tag->push_failures - 1 < 6 ? tag->push_failures - 1 : 6;
            int retry = RETRY_SECONDS << shift;
            retry = retry < MAX_RETRY_SECONDS ? retry : MAX_RETRY_SECONDS;
            snprintf(tag->push_result, sizeof(tag->push_result), "failed: %s", err);
            /* poll again soon: the Timestamp still differs, so the image comes again */
            if (tag->next_poll > now + retry) {
                tag->next_poll = now + retry;
            }
            gw_log("%s: transfer failed (%s), retry in %d s", mac, err, retry);
        }
    }
    gw_unlock();
}

static void poll_tag(const char *mac)
{
    poll_t p = {0};
    gw_tag_t *tag = relock(mac);
    if (!tag) {
        return;
    }
    p.job = *tag;
    p.forced = tag->forced;
    tag->forced = false;
    tag->last_poll = gw_uptime();
    gw_unlock();

    char request[400];
    zo_build_request(request, sizeof(request), p.job.mac, p.job.sdk_type, p.job.battery_dv, p.job.rssi);
    zo_answer_t ans;
    char err[72] = "";
    int rc = zo_fetch(CONFIG_ZO_SERVER_URL, p.job.api_key, request, &ans, want_body, on_body, &p, err, sizeof(err));
    bool preview_ok = false;
    if (p.preview) {
        preview_ok = fclose(p.preview) == 0;
        p.preview = NULL;
        if (!preview_ok && !p.bad) {
            p.bad = true;
            snprintf(p.bad_reason, sizeof(p.bad_reason), "preview close failed");
        }
    }
    if (rc == 0 && p.want && !p.bad) {
        decode_body(&p, ans.rotate);
    }
    zo_format_t fmt = p.dec ? zo_decoder_format(p.dec) : ZO_FMT_NONE;
    zo_decoder_free(p.dec);
    gw_settings_t cfg = s_settings;
    int64_t now = gw_uptime();

    if (!(tag = relock(mac))) {
        goto out;
    }
    if (rc != 0) {
        snprintf(tag->poll_result, sizeof(tag->poll_result), "error: %s", err);
        tag->next_poll = now + ERROR_RETRY_SECONDS;
        gw_unlock();
        gw_log("%s: zivyobraz.eu failed: %s", mac, err);
        goto out;
    }
    int sleep = zo_answer_sleep(&ans);
    if (sleep < 0) {
        sleep = ERROR_RETRY_SECONDS;
    }
    sleep = sleep < cfg.min_interval ? cfg.min_interval : sleep > cfg.max_interval ? cfg.max_interval : sleep;
    tag->next_poll = now + sleep;
    if (tag->rotate != ans.rotate) {
        tag->rotate = ans.rotate;
        s_dirty = true;
    }
    if (!(p.changed || p.forced)) {
        snprintf(tag->poll_result, sizeof(tag->poll_result), "ok, unchanged");
        gw_unlock();
        gw_log("%s: unchanged (timestamp %s, next in %d s)", mac, ans.timestamp[0] ? ans.timestamp : "-", sleep);
        goto out;
    }
    if (p.bad) {
        snprintf(tag->poll_result, sizeof(tag->poll_result), "bad image: %s", p.bad_reason);
        gw_unlock();
        gw_log("%s: can't use the image: %s (timestamp %s)", mac, p.bad_reason, ans.timestamp);
        goto out;
    }
    if (preview_ok) { /* the whole body is in the preview file */
        char path[48];
        gw_preview_path(mac, path, sizeof(path));
        remove_previews(mac);
        if (rename(p.tmp, path) == 0) {
            tag->preview_version = ++s_preview_counter;
        }
    }
    const char *reason = p.changed ? "ok, new image" : "ok, forced";
    snprintf(tag->poll_result, sizeof(tag->poll_result), "%s", reason);
    gw_unlock();
    gw_log("%s: %s (timestamp %s, %s %u B, next in %d s)", mac, p.changed ? "new image" : "forced refresh",
           ans.timestamp[0] ? ans.timestamp : "-", zo_format_name(fmt), (unsigned)p.preview_len, sleep);
    transfer(&p, ans.timestamp, reason);

out:
    if (p.tmp[0]) {
        remove(p.tmp); /* (gone already if it became the preview) */
    }
    ps_image_free(p.img);
    remove(WORK_IMG);
}

/* ---- the task ------------------------------------------------------------------------- */

static void gateway_task(void *arg)
{
    int64_t last_save = gw_uptime();
    bool online = false;
    for (;;) {
        if (!eth_has_ip()) {
            if (online) {
                gw_log("network down");
            }
            online = false;
            status_led_set(STATUS_NO_NET);
            eth_wait_ip(5000);
            continue;
        }
        if (!online) {
            char ip[40];
            eth_status(ip, sizeof(ip));
            gw_log("network up, %s", ip);
            online = true;
        }
        status_led_set(STATUS_OK);

        char mac[18] = "";
        int64_t now = gw_uptime();
        gw_lock();
        for (int i = 0; i < s_ntags; i++) {
            if (s_tags[i].next_poll <= now) {
                memcpy(mac, s_tags[i].mac, sizeof(mac));
                break;
            }
        }
        gw_unlock();
        if (mac[0]) {
            poll_tag(mac);
            continue;
        }

        update_service(); /* firmware update check / install (update.h), between polls */
        forget_old();
        if (s_dirty && gw_uptime() - last_save > SAVE_EVERY_SECONDS) {
            gw_lock();
            save();
            gw_unlock();
            last_save = gw_uptime();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

void gw_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_log_lock = xSemaphoreCreateMutex();
    mkdir(PREVIEW_DIR, 0755);
    load_settings();
    load();
    gw_log("%d tag(s) in " TAGS_FILE, s_ntags);
}

void gw_start(void)
{
    esp_err_t err = psb_init(on_advert);
    if (err != ESP_OK) {
        gw_log("Bluetooth failed to start: %s", esp_err_to_name(err));
    }
    /* TLS needs a deep stack */
    xTaskCreate(gateway_task, "gateway", 10240, NULL, 5, &s_task);
}
