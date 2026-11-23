/*
 * The web page (port of python/zivyobraz_ble_gateway/web.py, same HTML and the shared ../web/style.css):
 * registered tags, tags in range (with Add), recent log. Plain forms that POST and redirect
 * back; the page reloads itself every 15 s. No password: it's for a home LAN.
 *
 * ESP32 extras on /settings (no auto-reload there): poll intervals, tags.json backup and
 * restore (also takes the Python gateway's data/tags.json), firmware update.
 *
 * No password, but every request goes through guarded() (guard.h): only our own host names
 * (against DNS rebinding), POSTs only from this page (against cross-site forms and fetch()),
 * and the two uploads also need the X-ZO-Upload header that only the page's fetch() sends.
 */
#include "web.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway.h"
#include "guard.h"
#include "update.h"
#include "zivyobraz.h"

static const char *TAG = "web";

extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");

static char s_css_ver[9];

/* ---- chunked output ------------------------------------------------------------------- */

typedef struct {
    httpd_req_t *req;
    size_t n;
    char buf[1400];
} out_t;

static void flush(out_t *o)
{
    if (o->n) {
        httpd_resp_send_chunk(o->req, o->buf, (ssize_t)o->n);
        o->n = 0;
    }
}

static void put(out_t *o, const char *s)
{
    size_t len = strlen(s);
    while (len) {
        size_t room = sizeof(o->buf) - o->n;
        size_t take = len < room ? len : room;
        memcpy(o->buf + o->n, s, take);
        o->n += take, s += take, len -= take;
        if (o->n == sizeof(o->buf)) {
            flush(o);
        }
    }
}

static void putf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(out_t *o, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    put(o, tmp);
}

static void esc(out_t *o, const char *s)
{
    char one[2] = {0};
    for (; *s; s++) {
        switch (*s) {
        case '&': put(o, "&amp;"); break;
        case '<': put(o, "&lt;"); break;
        case '>': put(o, "&gt;"); break;
        case '"': put(o, "&quot;"); break;
        case '\'': put(o, "&#x27;"); break;
        default: one[0] = *s; put(o, one);
        }
    }
}

static void finish(out_t *o)
{
    flush(o);
    httpd_resp_send_chunk(o->req, NULL, 0);
}

/* ---- pieces of the page (web.py) ------------------------------------------------------- */

static const char *BLACK = "#111", *WHITE = "#fff", *RED = "#d7263d", *YELLOW = "#f4c20d";

static void duration(int64_t seconds, char *out, size_t size)
{
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds < 90) {
        snprintf(out, size, "%d s", (int)seconds);
    } else if (seconds < 5400) {
        snprintf(out, size, "%d min", (int)((seconds + 30) / 60));
    } else {
        snprintf(out, size, "%.1f h", seconds / 3600.0);
    }
}

static void swatches(out_t *o, int sdk)
{
    const char *ct = ps_color_type(sdk);
    const char *colours[4] = {BLACK, WHITE, RED, YELLOW};
    int n = strcmp(ct, "BW") == 0 ? 2 : strcmp(ct, "3C") == 0 ? 3 : 4;
    const char *name = n == 2 ? "Black / white" : n == 3 ? "Black / white / red" : "Black / white / red / yellow";
    put(o, "<span class=sw>");
    for (int i = 0; i < n; i++) {
        putf(o, "<i style='background:%s'></i>", colours[i]);
    }
    putf(o, "</span>%s", name);
}

static void status(out_t *o, const char *text)
{
    if (!text[0]) {
        put(o, "<span class='st none'>–</span>");
        return;
    }
    putf(o, "<span class='st %s'>", strncmp(text, "ok", 2) == 0 ? "ok" : "bad");
    esc(o, text);
    put(o, "</span>");
}

static void battery(out_t *o, int dv)
{
    if (dv) {
        putf(o, "<span class=batt>%d.%d V</span>", dv / 10, dv % 10);
    } else {
        put(o, "<span class='batt none'>–</span>");
    }
}

static void head(out_t *o, int sdk)
{
    const ps_model_t *m = ps_model(sdk);
    const char *desc = m ? m->desc : "";
    put(o, "<div class=spec>");
    /* "4.2in 400x300 BWR" -> 4.2 */
    size_t n = strspn(desc, "0123456789.");
    if (n && strncmp(desc + n, "in", 2) == 0) {
        putf(o, "<div class=inch>%.*s<span>&Prime;</span></div>", (int)n, desc);
    }
    const char *kind = ps_is_bw(sdk) ? "BW" : ps_is_bwry(sdk) ? "BWRY" : "BWR";
    putf(o, "<div class=specs><b class=panel>Picksmart %s%s</b><span>%d &times; %d px · type %d</span></div></div>",
         kind, strstr(desc, "TFT") ? " TFT" : "", m ? m->width : 0, m ? m->height : 0, sdk);
}

static void preview_name(const char *mac, char out[13])
{
    int j = 0;
    for (const char *p = mac; *p && j < 12; p++) {
        if (*p != ':') {
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

static void tag_card(out_t *o, const gw_tag_t *e, bool in_range, const char *busy)
{
    char w[40], name[13];
    put(o, "<li>");
    head(o, e->sdk_type);
    put(o, "<div class=mac>");
    esc(o, e->mac);
    put(o, "</div>");
    if (e->preview_version) {
        preview_name(e->mac, name);
        putf(o, "<img class=preview src='/preview/%s.bmp?v=%lu' alt=''%s>", name,
             (unsigned long)e->preview_version, e->rotate ? " style='transform:rotate(180deg)'" : "");
    }
    put(o, "<div class=key>zivyobraz key <b>");
    esc(o, e->api_key);
    put(o, "</b></div><dl>");
    put(o, "<dt>Colours</dt><dd>");
    swatches(o, e->sdk_type);
    put(o, "</dd><dt>Battery</dt><dd>");
    battery(o, e->battery_dv);
    if (in_range) {
        putf(o, "</dd><dt>Signal</dt><dd>%d dBm</dd>", e->rssi);
    } else {
        put(o, "</dd><dt>Signal</dt><dd>not in range</dd>");
    }
    gw_when(e->last_seen, w, sizeof(w));
    putf(o, "<dt>Last seen</dt><dd>%s</dd><dt>Server</dt><dd>", w);
    status(o, e->poll_result);
    gw_when(e->last_poll, w, sizeof(w));
    putf(o, "</dd><dt>Asked</dt><dd>%s</dd>", w);
    duration(e->next_poll - gw_uptime(), w, sizeof(w));
    putf(o, "<dt>Next</dt><dd>in %s</dd><dt>Transfer</dt><dd>", w);
    if (strcmp(busy, e->mac) == 0) {
        put(o, "<span class='st ok'>sending…</span>");
    } else {
        status(o, e->push_result);
    }
    gw_when(e->last_push, w, sizeof(w));
    putf(o, "</dd><dt>Sent</dt><dd>%s</dd><dt>Content</dt><dd>", w);
    esc(o, e->timestamp[0] ? e->timestamp : "–");
    putf(o, "%s</dd></dl><div class=actions>", e->rotate ? " · rotated" : "");
    putf(o, "<form method=post action=/refresh><input type=hidden name=mac value='%s'>"
            "<button class='btn small'>Refresh now</button></form>", e->mac);
    putf(o, "<form method=post action=/remove onsubmit=\"return confirm('Remove %s?')\">"
            "<input type=hidden name=mac value='%s'><button class='btn small'>Remove</button></form></div></li>",
         e->mac, e->mac);
}

static void seen_card(out_t *o, const gw_seen_t *s)
{
    const psb_advert_t *a = &s->adv;
    bool known = ps_model(a->adv.sdk_type) != NULL;
    put(o, "<li>");
    head(o, a->adv.sdk_type);
    putf(o, "<div class=mac>%s</div><dl><dt>Colours</dt><dd>", a->mac);
    if (known) {
        swatches(o, a->adv.sdk_type);
    } else {
        put(o, "–");
    }
    put(o, "</dd><dt>Battery</dt><dd>");
    battery(o, a->adv.battery_dv);
    putf(o, "</dd><dt>Signal</dt><dd>%d dBm</dd><dt>Firmware</dt><dd>0x%02X / hw 0x%02X</dd></dl>", a->rssi,
         a->adv.sw, a->adv.hw);
    if (known) {
        putf(o, "<form method=post action=/add><input type=hidden name=mac value='%s'>"
                "<button class='btn small'>Add</button></form></li>", a->mac);
    } else {
        put(o, "<p>Unknown display type: can't be served yet.</p></li>");
    }
}

static void log_line(void *ctx, const char *line)
{
    out_t *o = ctx;
    const char *text = strlen(line) > 9 ? line + 9 : "";
    const char *cls = (strstr(text, "failed") || strstr(text, "can't") || strstr(text, "error")) ? "err"
                      : (strstr(text, "image on the tag") || strncmp(text, "added", 5) == 0) ? "ok"
                                                                                               : "";
    putf(o, "<span class=t>%.8s</span> <span class='%s'>", line, cls);
    esc(o, text);
    put(o, "</span>\n");
}

/* refresh: reload every N s (0 = no) */
static void page_top(out_t *o, int refresh, const char *title)
{
    httpd_resp_set_type(o->req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(o->req, "Cache-Control", "no-store");
    put(o, "<!doctype html><html lang=en><head><meta charset=utf-8>");
    if (refresh) {
        putf(o, "<meta http-equiv=refresh content=%d>", refresh);
    }
    putf(o, "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<meta name=theme-color content='#009baa'><title>Živý obraz %s</title>"
            "<link rel=stylesheet href=/style.css?v=%s></head><body>",
         title, s_css_ver);
    putf(o, "<header class=top><a class=brand href=/>Laska<b>Kit</b></a>"
            "<div class=title>Živý obraz<b>%s</b></div></header><main>",
         title);
}

static void fmt_bytes(char *out, size_t size, size_t n)
{
    if (n >= 1024 * 1024) {
        snprintf(out, size, "%.1f MB", n / (1024.0 * 1024.0));
    } else if (n >= 1024) {
        snprintf(out, size, "%.1f kB", n / 1024.0);
    } else {
        snprintf(out, size, "%u B", (unsigned)n);
    }
}

static void page_bottom(out_t *o)
{
    char ip[40];
    upd_status_t u;
    eth_status(ip, sizeof(ip));
    update_status(&u);
    putf(o, "</main><footer>Živý obraz BLE Gateway · ESP32 %s · %s · ", esp_app_get_description()->version, ip);
    if (u.state == UPD_AVAILABLE) {
        put(o, "<a href=/settings><b>firmware ");
        esc(o, u.latest);
        put(o, " available</b></a> · ");
    }
    put(o, "<a href=/settings>gateway settings</a></footer></body></html>");
}

static int by_rssi(const void *a, const void *b)
{
    return ((const gw_seen_t *)b)->adv.rssi - ((const gw_seen_t *)a)->adv.rssi;
}

static esp_err_t index_get(httpd_req_t *req)
{
    out_t *o = calloc(1, sizeof(out_t));
    int ntags, nseen;
    gw_tag_t *tags = gw_tags_copy(&ntags);
    gw_seen_t *seen = gw_seen_copy(&nseen);
    if (!o || !tags || !seen) {
        free(o), free(tags), free(seen);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    o->req = req;
    char busy[18];
    gw_busy(busy);
    page_top(o, 15, "BLE Gateway");

    putf(o, "<section class=card><h2>Tags <small>%d</small></h2>", ntags);
    if (ntags) {
        put(o, "<p class=hint>Add each tag on <a href='https://zivyobraz.eu/' target=_blank>zivyobraz.eu</a> "
               "with its address (MAC) and its <b>zivyobraz key</b>.</p><ul class=displays>");
        for (int i = 0; i < ntags; i++) {
            bool in_range = false;
            for (int j = 0; j < nseen; j++) {
                in_range |= strcmp(seen[j].adv.mac, tags[i].mac) == 0;
            }
            tag_card(o, &tags[i], in_range, busy);
        }
        put(o, "</ul>");
    } else {
        put(o, "<p class=empty>No tags yet. Add one from the list below.</p>");
    }
    put(o, "</section>");

    /* in range, not added; strongest first */
    int nnew = 0;
    for (int j = 0; j < nseen; j++) {
        bool registered = false;
        for (int i = 0; i < ntags; i++) {
            registered |= strcmp(seen[j].adv.mac, tags[i].mac) == 0;
        }
        if (!registered) {
            seen[nnew++] = seen[j];
        }
    }
    qsort(seen, (size_t)nnew, sizeof(gw_seen_t), by_rssi);
    putf(o, "<section class=card><h2>In range, not added <small>%d</small></h2>", nnew);
    if (nnew) {
        put(o, "<ul class=displays>");
        for (int j = 0; j < nnew; j++) {
            seen_card(o, &seen[j]);
        }
        put(o, "</ul>");
    } else {
        put(o, "<p class=empty>No other Picksmart tags heard yet.</p>");
    }
    put(o, "</section>");

    put(o, "<section class='card log'><h2>Log <small>newest first</small></h2><pre>");
    size_t before = o->n;
    gw_log_each(log_line, o);
    if (o->n == before) {
        put(o, "(nothing yet)");
    }
    put(o, "</pre></section>");
    page_bottom(o);
    finish(o);
    free(tags), free(seen), free(o);
    return ESP_OK;
}

/* ---- /settings ------------------------------------------------------------------------- */

static const char *UPLOAD_JS =
    "<script>function up(id,url,back){var f=document.getElementById(id).files[0],m=document.getElementById(id+'m');"
    "if(!f){m.textContent='Choose a file first.';return}m.textContent='Uploading…';"
    "fetch(url,{method:'POST',headers:{'X-ZO-Upload':'1'},body:f}).then(function(r){return r.text()}).then(function(t){m.textContent=t;"
    "if(back)setTimeout(function(){location.href='/'},back)}).catch(function(e){m.textContent='Failed: '+e})}"
    "</script>";

/* GitHub releases (update.h) */
static void update_section(out_t *o, const upd_status_t *u)
{
    const char *running = esp_app_get_description()->version;
    if (!u->signed_app) {
        put(o, "<p class=hint><b>This firmware isn't signed</b> (a development build), so it can't check an "
               "update and refuses every one. Flash a release's <b>-full.bin</b> over USB.</p>");
    }
    if (u->state == UPD_OFF) {
        put(o, "<p class=hint>No release repository is set (menuconfig: ZO_OTA_REPO).</p>");
        return;
    }
    char w[40];
    put(o, "<p>Releases from <a href='https://github.com/" CONFIG_ZO_OTA_REPO "/releases' target=_blank>"
           CONFIG_ZO_OTA_REPO "</a>: ");
    switch (u->state) {
    case UPD_IDLE: put(o, "not checked yet."); break;
    case UPD_CHECKING: put(o, "checking…"); break;
    case UPD_LATEST:
        gw_when(u->checked, w, sizeof(w));
        put(o, "<span class='st ok'>");
        esc(o, running);
        putf(o, " is the newest</span> (checked %s).", w);
        break;
    case UPD_AVAILABLE:
        put(o, "<span class='st ok'><b>");
        esc(o, u->latest);
        put(o, "</b> is available</span> (this is ");
        esc(o, running);
        put(o, ").");
        break;
    case UPD_INSTALLING:
        put(o, "installing ");
        esc(o, u->latest);
        if (u->total) {
            putf(o, ": %u of %u kB…", (unsigned)(u->done / 1024), (unsigned)(u->total / 1024));
        } else {
            put(o, ": downloading…");
        }
        break;
    case UPD_RESTARTING: put(o, "installed, restarting. This page comes back in about 20 s."); break;
    case UPD_FAILED:
        put(o, "<span class='st bad'>");
        esc(o, u->message);
        put(o, "</span>");
        break;
    default: break;
    }
    put(o, "</p>");
    if (u->state == UPD_CHECKING || u->state == UPD_INSTALLING || u->state == UPD_RESTARTING) {
        return;
    }
    put(o, "<div class=actions>");
    if (u->state == UPD_AVAILABLE && u->signed_app) {
        put(o, "<form method=post action=/update/install onsubmit=\"return confirm('Install ");
        esc(o, u->latest);
        put(o, " and restart?')\"><button class='btn small'>Install ");
        esc(o, u->latest);
        put(o, "</button></form>");
    }
    put(o, "<form method=post action=/update/check><button class='btn small'>Check now</button></form></div>"
           "<p class=hint>Only a release signed with this firmware's key is installed. "
           "If the new firmware doesn't start, the gateway goes back to this one.</p>");
}

static esp_err_t settings_get(httpd_req_t *req)
{
    out_t *o = calloc(1, sizeof(out_t));
    if (!o) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    o->req = req;
    gw_settings_t s = gw_get_settings();
    upd_status_t u;
    update_status(&u);
    bool busy = u.state == UPD_CHECKING || u.state == UPD_INSTALLING || u.state == UPD_RESTARTING;
    page_top(o, busy ? 3 : 0, "BLE Gateway settings");

    put(o, "<section class=card><h2>Polling</h2><form method=post action=/settings class=cfg>");
    putf(o, "<label>Ask zivyobraz.eu at most every <input type=number name=min min=10 value=%d> s per tag</label>"
            "<label>… and at least every <input type=number name=max min=10 value=%d> s</label>"
            "<button class='btn small'>Save</button></form>"
            "<p class=hint>Between these limits the gateway follows what the server asks for (PreciseSleep).</p>"
            "</section>",
         s.min_interval, s.max_interval);

    put(o, "<section class=card><h2>Tag list backup</h2>"
           "<p class=hint>The zivyobraz keys in it are what zivyobraz.eu knows your tags by: keep a copy. "
           "The Python gateway's <b>data/tags.json</b> can be restored here too.</p>"
           "<div class=actions><a class='btn small' href=/tags.json download=tags.json>Download tags.json</a></div>"
           "<div class=cfg><input type=file id=tj accept='.json,application/json'>"
           "<button class='btn small' onclick=\"up('tj','/import',1500)\">Restore</button><p id=tjm class=hint></p></div>"
           "</section>");

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char ip[40], ram[96], flash[80];
    eth_status(ip, sizeof(ip));
    int64_t up = gw_uptime();
    {
        char free_s[16], big_s[16], low_s[16];
        fmt_bytes(free_s, sizeof(free_s), heap_caps_get_free_size(MALLOC_CAP_8BIT));
        fmt_bytes(big_s, sizeof(big_s), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        fmt_bytes(low_s, sizeof(low_s), heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        snprintf(ram, sizeof(ram), "%s free, largest block %s, lowest %s", free_s, big_s, low_s);
    }
    {
        size_t total = 0, used = 0;
        char used_s[16], tot_s[16];
        if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
            fmt_bytes(used_s, sizeof(used_s), used);
            fmt_bytes(tot_s, sizeof(tot_s), total);
            snprintf(flash, sizeof(flash), "%s used of %s", used_s, tot_s);
        } else {
            snprintf(flash, sizeof(flash), "unavailable");
        }
    }
    putf(o, "<section class=card><h2>Gateway</h2><ul class=displays><li><dl>"
            "<dt>Firmware</dt><dd>%s (%s %s)</dd><dt>Partition</dt><dd>%s</dd>"
            "<dt>Network</dt><dd>%s · " CONFIG_ZO_HOSTNAME ".local</dd>"
            "<dt>Uptime</dt><dd>%dd %02d:%02d</dd><dt>RAM</dt><dd>%s</dd>"
            "<dt>Storage</dt><dd>%s</dd><dt>Server</dt><dd>",
         app->version, app->date, app->time, run ? run->label : "?", ip, (int)(up / 86400), (int)(up / 3600 % 24),
         (int)(up / 60 % 60), ram, flash);
    esc(o, CONFIG_ZO_SERVER_URL);
    put(o, "</dd></dl></li></ul>");
    put(o, "<h2>Firmware update</h2>");
    update_section(o, &u);
#if CONFIG_ZO_WEB_OTA
    put(o, "<p class=hint>Or upload <b>zivyobraz-ble-gateway-esp32-&lt;version&gt;-ota.bin</b> from a release. "
           "The gateway restarts; if the new firmware doesn't start, it goes back to this one.</p>"
           "<div class=cfg><input type=file id=fw accept='.bin'>"
           "<button class='btn small' onclick=\"up('fw','/update',20000)\">Upload</button><p id=fwm class=hint></p></div>");
#endif
    put(o, "</section>");
    put(o, UPLOAD_JS);
    page_bottom(o);
    finish(o);
    free(o);
    return ESP_OK;
}

/* ---- the guard (guard.h) --------------------------------------------------------------- */

typedef struct {
    esp_err_t (*fn)(httpd_req_t *req);
    const char *what; /* action_post: add / remove / refresh */
    bool upload;      /* POST only from the page's fetch(), which adds X-ZO-Upload */
} route_t;

/* 1 = got it, 0 = not sent, -1 = too long */
static int header(httpd_req_t *req, const char *name, char *out, size_t size)
{
    size_t n = httpd_req_get_hdr_value_len(req, name);
    out[0] = 0;
    if (!n) {
        return 0;
    }
    if (n >= size || httpd_req_get_hdr_value_str(req, name, out, size) != ESP_OK) {
        return -1;
    }
    return 1;
}

static esp_err_t refuse(httpd_req_t *req, const char *why, const char *host, const char *origin)
{
    gw_log("web: refused %s %.40s (%s; host %.40s, origin %.40s)", req->method == HTTP_POST ? "POST" : "GET",
           req->uri, why, host[0] ? host : "-", origin[0] ? origin : "-");
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Refused: open the gateway's page by its own address and use the buttons there.");
}

static esp_err_t guarded(httpd_req_t *req)
{
    const route_t *r = req->user_ctx;
    char host[80], origin[160], flag[8];
    if (header(req, "Host", host, sizeof(host)) != 1 || !guard_host_ok(host, CONFIG_ZO_HOSTNAME)) {
        return refuse(req, "unknown host name", host, "");
    }
    if (req->method == HTTP_POST) {
        int got = header(req, "Origin", origin, sizeof(origin));
        if (got == 0) {
            got = header(req, "Referer", origin, sizeof(origin));
        }
        if (got < 0 || !guard_origin_ok(origin, host)) {
            return refuse(req, "from another site", host, origin);
        }
        if (r->upload && header(req, "X-ZO-Upload", flag, sizeof(flag)) != 1) {
            return refuse(req, "upload without X-ZO-Upload", host, origin);
        }
    }
    return r->fn(req);
}

/* ---- forms ---------------------------------------------------------------------------- */

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r == '+' ? ' ' : *r;
        }
    }
    *w = 0;
}

static bool form_value(httpd_req_t *req, const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

/* httpd serves one request at a time: a client that stops sending mustn't hold it for ever.
 * Each timeout is the server's recv_wait_timeout (5 s). */
#define MAX_RECV_TIMEOUTS 3

/* The whole body into buf (NUL-terminated). -1 if it doesn't fit or doesn't arrive. */
static int read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) {
        return -1;
    }
    size_t want = req->content_len, got = 0;
    int timeouts = 0;
    while (got < want) {
        int n = httpd_req_recv(req, buf + got, want - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    buf[got] = 0;
    return (int)got;
}

static esp_err_t see_other(httpd_req_t *req, const char *where)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", where);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t action_post(httpd_req_t *req)
{
    /* Browsers send "mac=FF%3AFF%3A..." (27 bytes encoded). httpd_query_key_value needs
     * room for the encoded value, so this has to be larger than a decoded MAC. */
    char body[256], mac[64];
    if (read_body(req, body, sizeof(body)) < 0 || !form_value(req, body, "mac", mac, sizeof(mac))) {
        ESP_LOGW(TAG, "POST %s: missing mac", req->uri);
        return see_other(req, "/");
    }
    for (char *p = mac; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    const char *what = ((const route_t *)req->user_ctx)->what;
    char err[96];
    if (strcmp(what, "add") == 0) {
        if (!gw_add(mac, err, sizeof(err))) {
            gw_log("can't: %s", err);
        }
    } else if (strcmp(what, "remove") == 0) {
        gw_remove(mac);
    } else {
        gw_refresh(mac);
    }
    return see_other(req, "/");
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char body[128], v[16];
    if (read_body(req, body, sizeof(body)) >= 0) {
        gw_settings_t s = gw_get_settings();
        if (form_value(req, body, "min", v, sizeof(v))) {
            s.min_interval = atoi(v);
        }
        if (form_value(req, body, "max", v, sizeof(v))) {
            s.max_interval = atoi(v);
        }
        gw_set_settings(s);
    }
    return see_other(req, "/settings");
}

static esp_err_t update_check_post(httpd_req_t *req)
{
    update_request_check();
    return see_other(req, "/settings");
}

static esp_err_t update_install_post(httpd_req_t *req)
{
    if (!update_request_install()) {
        gw_log("update: nothing to install");
    }
    return see_other(req, "/settings");
}

/* ---- tags.json backup / restore -------------------------------------------------------- */

static esp_err_t export_get(httpd_req_t *req)
{
    char *text = gw_export();
    if (!text) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=tags.json");
    esp_err_t e = httpd_resp_sendstr(req, text);
    free(text);
    return e;
}

static esp_err_t import_post(httpd_req_t *req)
{
    if (req->content_len > 64 * 1024) {
        return httpd_resp_sendstr(req, "That file is too big for a tags.json.");
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf || read_body(req, buf, req->content_len + 1) < 0) {
        free(buf);
        return httpd_resp_sendstr(req, "Upload failed.");
    }
    int added;
    char err[64], msg[96];
    if (gw_import(buf, &added, err, sizeof(err))) {
        snprintf(msg, sizeof(msg), "Restored %d tag(s).", added);
    } else {
        snprintf(msg, sizeof(msg), "Not restored: %s.", err);
    }
    free(buf);
    return httpd_resp_sendstr(req, msg);
}

/* ---- firmware update ------------------------------------------------------------------- */

#if CONFIG_ZO_WEB_OTA
static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota = 0;
    if (!part || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return httpd_resp_sendstr(req, "Can't start the update.");
    }
    char *buf = malloc(4096);
    size_t left = req->content_len, done = 0;
    const char *problem = buf ? NULL : "out of memory";
    int timeouts = 0;
    while (left && !problem) {
        int n = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) {
            continue;
        }
        timeouts = 0;
        if (n <= 0) {
            problem = "upload interrupted";
            break;
        }
        if (done == 0) { /* refuse other programs: compare the project name */
            size_t at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            const esp_app_desc_t *new_app = (const esp_app_desc_t *)(buf + at);
            if ((size_t)n < at + sizeof(esp_app_desc_t) || new_app->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strcmp(new_app->project_name, esp_app_get_description()->project_name) != 0) {
                problem = "not a zivyobraz-ble-gateway firmware file";
                break;
            }
        }
        if (esp_ota_write(ota, buf, (size_t)n) != ESP_OK) {
            problem = "flash write failed";
            break;
        }
        done += (size_t)n, left -= (size_t)n;
    }
    free(buf);
    if (problem) {
        esp_ota_abort(ota);
        char msg[80];
        snprintf(msg, sizeof(msg), "Update failed: %s.", problem);
        return httpd_resp_sendstr(req, msg);
    }
    esp_err_t e = esp_ota_end(ota);
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        return httpd_resp_sendstr(req, "Update failed: the file isn't signed with this gateway's key, or it is damaged.");
    }
    if (e != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        return httpd_resp_sendstr(req, "Update failed: the file is damaged.");
    }
    gw_log("firmware update received (%u B), restarting", (unsigned)done);
    httpd_resp_sendstr(req, "Updated. The gateway restarts now; this page reloads in 20 s.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
#endif

/* ---- static files and previews -------------------------------------------------------- */

static esp_err_t static_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start);
}

/* no icon: answer 204 rather than letting the page's wildcard handler serve HTML */
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* A stored Z2/Z3 body drawn as a 4-bit BMP (top-down), in the colours the panel shows. */
typedef struct {
    httpd_req_t *req;
    uint32_t w, h, row;
    size_t stride;
    bool bw;
    uint8_t *line;
} bmp_t;

static void bmp_next_row(bmp_t *b)
{
    httpd_resp_send_chunk(b->req, (const char *)b->line, (ssize_t)b->stride);
    memset(b->line, 0x00, b->stride); /* palette 0 = white */
    b->row++;
}

static void bmp_sink(void *ctx, uint32_t pos, uint32_t count, ps_color_t c)
{
    bmp_t *b = ctx;
    if (b->bw && c == PS_RED) {
        c = PS_BLACK;
    }
    for (uint32_t p = pos; p < pos + count; p++) {
        uint32_t y = p / b->w, x = p % b->w;
        while (b->row < y && b->row < b->h) {
            bmp_next_row(b);
        }
        b->line[x / 2] |= (uint8_t)(x & 1 ? c : c << 4);
    }
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static esp_err_t send_bmp(httpd_req_t *req, FILE *f, int sdk)
{
    const ps_model_t *m = ps_model(sdk);
    bmp_t b = {.req = req, .w = m->width, .h = m->height, .bw = ps_is_bw(sdk)};
    b.stride = ((b.w * 4 + 31) / 32) * 4;
    b.line = calloc(1, b.stride);
    uint8_t *chunk = malloc(512);
    zo_decoder_t *d = zo_decoder_new(sdk, b.w, b.h, bmp_sink, &b);
    if (!b.line || !chunk || !d) {
        free(b.line), free(chunk), zo_decoder_free(d);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    uint8_t hdr[14 + 40 + 16] = {'B', 'M'};
    uint32_t data_off = sizeof(hdr);
    le32(hdr + 2, data_off + (uint32_t)b.stride * b.h);
    le32(hdr + 10, data_off);
    le32(hdr + 14, 40);
    le32(hdr + 18, b.w);
    le32(hdr + 22, (uint32_t)-(int32_t)b.h); /* negative: top-down */
    hdr[26] = 1, hdr[28] = 4;               /* planes, bits per pixel */
    le32(hdr + 46, 4);                       /* colours used */
    const uint8_t pal[4][4] = {{255, 255, 255, 0}, {0, 0, 0, 0}, {0x3d, 0x26, 0xd7, 0}, {0x0d, 0xc2, 0xf4, 0}}; /* BGR0 */
    memcpy(hdr + 54, pal, sizeof(pal));
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000");
    httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    size_t n;
    while ((n = fread(chunk, 1, 512, f)) > 0 && zo_decoder_feed(d, chunk, n)) {
    }
    while (b.row < b.h) {
        bmp_next_row(&b); /* the rest (and any short image) is white */
    }
    httpd_resp_send_chunk(req, NULL, 0);
    zo_decoder_free(d);
    free(b.line), free(chunk);
    return ESP_OK;
}

static esp_err_t preview_get(httpd_req_t *req)
{
    /* /preview/FFFF92813440.bmp?v=3 */
    const char *name = req->uri + strlen("/preview/");
    char hexmac[13] = {0};
    if (sscanf(name, "%12[0-9A-F]", hexmac) != 1 || strlen(hexmac) != 12) {
        return httpd_resp_send_404(req);
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", hexmac, hexmac + 2, hexmac + 4, hexmac + 6,
             hexmac + 8, hexmac + 10);
    int ntags, sdk = -1;
    gw_tag_t *tags = gw_tags_copy(&ntags);
    for (int i = 0; tags && i < ntags; i++) {
        if (strcmp(tags[i].mac, mac) == 0) {
            sdk = tags[i].sdk_type;
        }
    }
    free(tags);
    char path[48];
    gw_preview_path(mac, path, sizeof(path));
    FILE *f = sdk >= 0 && ps_model(sdk) ? fopen(path, "rb") : NULL;
    if (!f) {
        return httpd_resp_send_404(req);
    }
    esp_err_t e = send_bmp(req, f, sdk);
    fclose(f);
    return e;
}

/* ---- server --------------------------------------------------------------------------- */

static void fnv(const uint8_t *d, size_t n, char out[9])
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ d[i]) * 16777619u;
    }
    snprintf(out, 9, "%08lx", (unsigned long)h);
}

esp_err_t web_start(void)
{
    fnv(style_css_start, (size_t)(style_css_end - style_css_start), s_css_ver);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }
    static const struct {
        const char *uri;
        httpd_method_t method;
        route_t route;
    } uris[] = {
        {"/", HTTP_GET, {.fn = index_get}},
        {"/settings", HTTP_GET, {.fn = settings_get}},
        {"/settings", HTTP_POST, {.fn = settings_post}},
        {"/style.css", HTTP_GET, {.fn = static_get}},
        {"/favicon.ico", HTTP_GET, {.fn = favicon_get}},
        {"/preview/*", HTTP_GET, {.fn = preview_get}},
        {"/add", HTTP_POST, {.fn = action_post, .what = "add"}},
        {"/remove", HTTP_POST, {.fn = action_post, .what = "remove"}},
        {"/refresh", HTTP_POST, {.fn = action_post, .what = "refresh"}},
        {"/tags.json", HTTP_GET, {.fn = export_get}},
        {"/update/check", HTTP_POST, {.fn = update_check_post}},
        {"/update/install", HTTP_POST, {.fn = update_install_post}},
        {"/import", HTTP_POST, {.fn = import_post, .upload = true}},
#if CONFIG_ZO_WEB_OTA
        {"/update", HTTP_POST, {.fn = update_post, .upload = true}},
#endif
        /* anything else (/index.html, ...): the page itself, not a redirect (see web.py) */
        {"/*", HTTP_GET, {.fn = index_get}},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        const httpd_uri_t u = {.uri = uris[i].uri, .method = uris[i].method, .handler = guarded,
                               .user_ctx = (void *)&uris[i].route};
        httpd_register_uri_handler(server, &u);
    }
    ESP_LOGI(TAG, "web page on http://" CONFIG_ZO_HOSTNAME ".local/");
    return ESP_OK;
}
