/*
 * Picksmart tags over NimBLE: the continuous scanner and the image transfer.
 * Written from PROTOCOL.md (§3 GATT, §4 transfer, §4.5 timeouts); it follows
 * python/picksmart/client.py step by step, with the same log lines.
 *
 * Threading: NimBLE callbacks (host task) only copy what they got into one of two queues:
 * `s_ops` (procedure results: found, connected, discovered, written, disconnected) and `s_rx`
 * (notifications). psb_send() runs the whole transfer sequentially in the caller's task.
 */
#include "picksmart_ble.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "picksmart_ble";

/* §4.5, in ms */
#define T_SCAN 20000
#define T_CONNECT 20000
#define CONNECT_ATTEMPTS 5
#define T_BACKOFF 2000 /* × attempt */
#define T_MTU 4000
#define T_DISCOVER 8000 /* SPEC-GAP: §4.5 has no GATT discovery timeout; the "single GATT write" one is used */
#define T_SETTLE 400
#define T_WRITE 8000
#define T_BLOCK_SIZE 4000
#define BLOCK_SIZE_TRIES 2
#define T_PREPARE 10000
#define T_FIRST_REQUEST 12000
#define T_LEGACY_NEXT 12000
#define T_DISCONNECT 5000
#define T_WHOLE 240000

/* §4.1 */
#define CMD_BLOCK_SIZE 0x01
#define CMD_PREPARE 0x02
#define CMD_START 0x03
#define MSG_DATA 0x05
#define DATA_REQUEST 0x00
#define DATA_DONE 0x08
#define BLOCK_HEADER 4
#define MAX_REQUESTS 5 /* §4.2 */
#define MAX_BLOCK 512

typedef enum { OP_FOUND, OP_CONNECTED, OP_DISCONNECTED, OP_MTU, OP_SVC, OP_CHR, OP_DSC, OP_DONE, OP_WRITTEN } op_kind_t;

typedef struct {
    op_kind_t kind;
    uint32_t seq; /* which procedure it belongs to (late answers of an old one are dropped) */
    int status;
    uint16_t a, b, c; /* SVC: start, end. CHR: def, val, uuid16. DSC: handle, uuid16. MTU: mtu */
    uint8_t props;
    ble_addr_t addr; /* FOUND */
} op_event_t;

typedef struct {
    uint8_t len;
    uint8_t data[23];
} rx_msg_t;

static psb_advert_fn s_on_advert;
static QueueHandle_t s_ops, s_rx;
static volatile bool s_synced, s_busy;
static uint8_t s_own_addr_type;
static volatile uint32_t s_seq = 1;
static volatile uint32_t s_conn_seq;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_finding;
static ble_addr_t s_find_addr;
static int64_t s_deadline; /* whole transfer, ms */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t next_seq(void)
{
    return ++s_seq;
}

static void post(const op_event_t *ev)
{
    if (xQueueSend(s_ops, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped event %d", ev->kind);
    }
}

/* ---- logging ---------------------------------------------------------------------------------- */

typedef struct {
    const psb_transfer_t *t;
    char *err;
    size_t err_size;
} job_t;

static void say(const job_t *j, const char *fmt, ...)
{
    char line[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s: %s", j->t->mac, line);
    if (j->t->log) {
        j->t->log(line);
    }
}

static int fail(const job_t *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->err, j->err_size, fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s: %s", j->t->mac, j->err);
    if (j->t->log) {
        j->t->log(j->err);
    }
    return -1;
}

/* ---- scanner ---------------------------------------------------------------------------------- */

static void format_mac(const ble_addr_t *a, char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1],
             a->val[0]);
}

static bool parse_mac(const char *mac, uint8_t type, ble_addr_t *out)
{
    unsigned v[6];
    if (sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    out->type = type;
    for (int i = 0; i < 6; i++) {
        out->val[5 - i] = (uint8_t)v[i];
    }
    return true;
}

/* The manufacturer data AD structure of an advertising report, if it is Picksmart's (§1). */
static bool find_mfg(const uint8_t *data, uint8_t len, ps_adv_t *out)
{
    for (size_t i = 0; i + 1 < len;) {
        uint8_t field = data[i];
        if (field == 0 || i + 1 + field > len) {
            break;
        }
        if (data[i + 1] == 0xFF && ps_parse_mfg(&data[i + 2], field - 1, out)) {
            return true;
        }
        i += 1 + (size_t)field;
    }
    return false;
}

static int scan_event(struct ble_gap_event *event, void *arg);

static void scan_start(int32_t duration_ms)
{
    struct ble_gap_disc_params p = {
        .itvl = 0x50, .window = 0x30, /* 50 ms / 30 ms */
        .passive = 0,                 /* active: names come in scan responses */
        .filter_duplicates = 0,       /* every advertisement: battery and signal stay fresh */
    };
    int rc = ble_gap_disc(s_own_addr_type, duration_ms, &p, scan_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
    }
}

static void scan_stop(void)
{
    ble_gap_disc_cancel();
}

static int scan_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *d = &event->disc;
        psb_advert_t a = {.addr_type = d->addr.type, .rssi = d->rssi};
        if (!find_mfg(d->data, d->length_data, &a.adv)) {
            return 0;
        }
        format_mac(&d->addr, a.mac);
        if (s_finding && memcmp(d->addr.val, s_find_addr.val, 6) == 0) {
            op_event_t ev = {.kind = OP_FOUND, .seq = s_seq, .addr = d->addr};
            s_finding = false;
            post(&ev);
        }
        if (s_on_advert) {
            s_on_advert(&a);
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (!s_busy && s_synced) { /* ended by itself (e.g. after an error): keep scanning */
            scan_start(BLE_HS_FOREVER);
        }
    }
    return 0;
}

/* ---- host ------------------------------------------------------------------------------------- */

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    s_synced = true;
    if (!s_busy) {
        scan_start(BLE_HS_FOREVER);
    }
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t psb_init(psb_advert_fn on_advert)
{
    s_on_advert = on_advert;
    s_ops = xQueueCreate(48, sizeof(op_event_t));
    s_rx = xQueueCreate(16, sizeof(rx_msg_t));
    if (!s_ops || !s_rx) {
        return ESP_ERR_NO_MEM;
    }
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(247); /* §3: the largest MTU */
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool psb_ready(void)
{
    return s_synced;
}

/* ---- connection callbacks (host task: copy into the queues, nothing else) --------------------- */

static int conn_event(struct ble_gap_event *event, void *arg)
{
    op_event_t ev = {.seq = s_conn_seq};
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ev.kind = OP_CONNECTED;
        ev.status = event->connect.status;
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
        }
        post(&ev);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        ev.kind = OP_DISCONNECTED;
        ev.status = event->disconnect.reason;
        post(&ev);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        rx_msg_t m = {0};
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        m.len = (uint8_t)(len < sizeof(m.data) ? len : sizeof(m.data));
        os_mbuf_copydata(event->notify_rx.om, 0, m.len, m.data);
        if (xQueueSend(s_rx, &m, 0) != pdTRUE) {
            ESP_LOGW(TAG, "notification queue full");
        }
        break;
    }
    default: break;
    }
    return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    op_event_t ev = {.kind = OP_MTU, .seq = (uint32_t)(uintptr_t)arg, .status = error->status, .a = mtu};
    post(&ev);
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    op_event_t ev = {.seq = (uint32_t)(uintptr_t)arg, .status = error->status};
    if (error->status == 0 && svc) {
        ev.kind = OP_SVC, ev.a = svc->start_handle, ev.b = svc->end_handle;
    } else {
        ev.kind = OP_DONE;
        ev.status = error->status == BLE_HS_EDONE ? 0 : error->status;
    }
    post(&ev);
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    op_event_t ev = {.seq = (uint32_t)(uintptr_t)arg, .status = error->status};
    if (error->status == 0 && chr) {
        ev.kind = OP_CHR, ev.a = chr->def_handle, ev.b = chr->val_handle, ev.c = ble_uuid_u16(&chr->uuid.u);
        ev.props = chr->properties;
    } else {
        ev.kind = OP_DONE;
        ev.status = error->status == BLE_HS_EDONE ? 0 : error->status;
    }
    post(&ev);
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    op_event_t ev = {.seq = (uint32_t)(uintptr_t)arg, .status = error->status};
    if (error->status == 0 && dsc) {
        ev.kind = OP_DSC, ev.a = dsc->handle, ev.b = ble_uuid_u16(&dsc->uuid.u);
    } else {
        ev.kind = OP_DONE;
        ev.status = error->status == BLE_HS_EDONE ? 0 : error->status;
    }
    post(&ev);
    return 0;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    op_event_t ev = {.kind = OP_WRITTEN, .seq = (uint32_t)(uintptr_t)arg, .status = error->status};
    post(&ev);
    return 0;
}

/* ---- waiting ---------------------------------------------------------------------------------- */

typedef enum { W_OK, W_TIMEOUT, W_LOST } wait_t;

static TickType_t ticks_until(int64_t until)
{
    int64_t left = until - now_ms();
    if (left > s_deadline - now_ms()) {
        left = s_deadline - now_ms();
    }
    return left > 0 ? pdMS_TO_TICKS(left) : 0;
}

/* The next event of procedure `seq`. A disconnect ends every wait except the one for it. */
static wait_t wait_op(uint32_t seq, int timeout_ms, op_event_t *out)
{
    int64_t until = now_ms() + timeout_ms;
    for (;;) {
        TickType_t ticks = ticks_until(until);
        if (!ticks || xQueueReceive(s_ops, out, ticks) != pdTRUE) {
            return W_TIMEOUT;
        }
        if (out->kind == OP_DISCONNECTED && out->seq == s_conn_seq) {
            return out->seq == seq ? W_OK : W_LOST;
        }
        if (out->seq == seq) {
            return W_OK;
        }
    }
}

static bool connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

/* ---- the session ------------------------------------------------------------------------------ */

typedef struct {
    job_t j;
    uint16_t conn;
    uint16_t ctrl, data, ctrl_cccd, data_cccd;
    uint8_t ctrl_props, data_props;
    uint16_t block_size;
    uint32_t count;
    size_t length;
    uint8_t *requests; /* per block, how often the tag asked for it */
    uint32_t blocks_sent;
} session_t;

static bool find_tag(session_t *s, ble_addr_t *addr)
{
    uint32_t seq = next_seq();
    s_finding = true;
    scan_start(T_SCAN);
    op_event_t ev;
    bool found = false;
    int64_t until = now_ms() + T_SCAN;
    while (!found && now_ms() < until) {
        if (wait_op(seq, (int)(until - now_ms()), &ev) != W_OK) {
            break;
        }
        if (ev.kind == OP_FOUND) {
            *addr = ev.addr;
            found = true;
        }
    }
    s_finding = false;
    scan_stop();
    return found;
}

static int connect(session_t *s)
{
    ble_addr_t addr;
    if (!parse_mac(s->j.t->mac, s->j.t->addr_type, &addr)) {
        return fail(&s->j, "bad address %s", s->j.t->mac);
    }
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS; attempt++) {
        if (now_ms() >= s_deadline) {
            break;
        }
        s_find_addr = addr;
        if (!find_tag(s, &addr)) { /* §4.5: rescan before each attempt */
            say(&s->j, "connect attempt %d failed: not seen within %d s", attempt, T_SCAN / 1000);
        } else {
            s_conn_seq = next_seq();
            int rc = ble_gap_connect(s_own_addr_type, &addr, T_CONNECT, NULL, conn_event, NULL);
            op_event_t ev;
            wait_t w = rc == 0 ? wait_op(s_conn_seq, T_CONNECT + 1000, &ev) : W_TIMEOUT;
            if (rc == 0 && w == W_OK && ev.kind == OP_CONNECTED && ev.status == 0) {
                s->conn = s_conn;
                say(&s->j, "connected (attempt %d)", attempt);
                return 0;
            }
            if (rc == 0 && w == W_TIMEOUT) {
                ble_gap_conn_cancel();
            }
            say(&s->j, "connect attempt %d failed: %d", attempt, rc ? rc : w == W_OK ? ev.status : -1);
        }
        if (attempt < CONNECT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(T_BACKOFF * attempt));
        }
    }
    return fail(&s->j, "can't connect to %s", s->j.t->mac);
}

static void exchange_mtu(session_t *s)
{
    uint32_t seq = next_seq();
    op_event_t ev;
    int rc = ble_gattc_exchange_mtu(s->conn, mtu_cb, (void *)(uintptr_t)seq);
    if (rc == 0 && wait_op(seq, T_MTU, &ev) == W_OK && ev.kind == OP_MTU && ev.status == 0) {
        say(&s->j, "MTU %u", ev.a);
    } else { /* §4.5: carry on with the default */
        say(&s->j, "MTU exchange failed, using %u", ble_att_mtu(s->conn));
    }
}

/* Wait for the end of a discovery, collecting its events with `each`. */
static int discover_wait(session_t *s, uint32_t seq, void (*each)(session_t *, const op_event_t *, void *), void *ctx)
{
    op_event_t ev;
    for (;;) {
        wait_t w = wait_op(seq, T_DISCOVER, &ev);
        if (w != W_OK) {
            return fail(&s->j, w == W_LOST ? "disconnected during discovery" : "GATT discovery timed out");
        }
        if (ev.kind == OP_DONE) {
            return ev.status == 0 ? 0 : fail(&s->j, "GATT discovery failed: %d", ev.status);
        }
        each(s, &ev, ctx);
    }
}

typedef struct {
    uint16_t start, end;
    bool found;
} svc_range_t;

static void on_svc(session_t *s, const op_event_t *ev, void *ctx)
{
    svc_range_t *r = ctx;
    if (ev->kind == OP_SVC && !r->found) {
        r->start = ev->a, r->end = ev->b, r->found = true;
    }
}

typedef struct {
    uint16_t base; /* 0xFEF0, 0xFDF0, ... */
    uint16_t def[2], end[2];
    uint16_t last_def;
    uint16_t svc_end;
} chr_scan_t;

static void on_chr(session_t *s, const op_event_t *ev, void *ctx)
{
    chr_scan_t *c = ctx;
    if (ev->kind != OP_CHR) {
        return;
    }
    /* the characteristic before this one ends where this one's declaration starts */
    for (int i = 0; i < 2; i++) {
        if (c->def[i] && c->def[i] == c->last_def && !c->end[i]) {
            c->end[i] = ev->a - 1;
        }
    }
    c->last_def = ev->a;
    if (ev->c == c->base + 1) {
        s->ctrl = ev->b, s->ctrl_props = ev->props, c->def[0] = ev->a;
    } else if (ev->c == c->base + 2) {
        s->data = ev->b, s->data_props = ev->props, c->def[1] = ev->a;
    }
}

static void on_dsc(session_t *s, const op_event_t *ev, void *ctx)
{
    uint16_t *cccd = ctx;
    if (ev->kind == OP_DSC && ev->b == BLE_GATT_DSC_CLT_CFG_UUID16 && !*cccd) {
        *cccd = ev->a;
    }
}

static int find_cccd(session_t *s, uint16_t val, uint16_t end, uint16_t *cccd)
{
    *cccd = 0;
    if (end > val) {
        uint32_t seq = next_seq();
        if (ble_gattc_disc_all_dscs(s->conn, val, end, dsc_cb, (void *)(uintptr_t)seq) == 0 &&
            discover_wait(s, seq, on_dsc, cccd) != 0) {
            return -1;
        }
    }
    if (!*cccd) { /* SPEC-GAP: §3 says nothing about the CCCD; the usual place right after the value */
        *cccd = val + 1;
        say(&s->j, "no CCCD found for handle %u, using %u", val, *cccd);
    }
    return 0;
}

/* §3: the FEF0 service (or FDF0, FCF0, FBF0) with FEF1 control and FEF2 data. */
static int discover(session_t *s)
{
    static const uint16_t bases[] = {0xFEF0, 0xFDF0, 0xFCF0, 0xFBF0};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        ble_uuid16_t uuid = BLE_UUID16_INIT(bases[i]);
        svc_range_t r = {0};
        uint32_t seq = next_seq();
        if (ble_gattc_disc_svc_by_uuid(s->conn, &uuid.u, svc_cb, (void *)(uintptr_t)seq) != 0 ||
            discover_wait(s, seq, on_svc, &r) != 0) {
            return -1;
        }
        if (!r.found) {
            continue;
        }
        chr_scan_t c = {.base = bases[i], .svc_end = r.end};
        s->ctrl = s->data = 0;
        seq = next_seq();
        if (ble_gattc_disc_all_chrs(s->conn, r.start, r.end, chr_cb, (void *)(uintptr_t)seq) != 0 ||
            discover_wait(s, seq, on_chr, &c) != 0) {
            return -1;
        }
        if (!s->ctrl || !s->data) {
            continue;
        }
        for (int k = 0; k < 2; k++) {
            if (!c.end[k]) {
                c.end[k] = r.end;
            }
        }
        if (find_cccd(s, s->ctrl, c.end[0], &s->ctrl_cccd) != 0 || find_cccd(s, s->data, c.end[1], &s->data_cccd) != 0) {
            return -1;
        }
        return 0;
    }
    return fail(&s->j, "no Picksmart service (FEF0/FDF0/FCF0/FBF0) on this device");
}

/* A write with response, `timeout_ms`. 0 ok, 1 timed out, -1 failed. */
static int write_acked(session_t *s, uint16_t handle, const uint8_t *data, size_t len)
{
    uint32_t seq = next_seq();
    if (ble_gattc_write_flat(s->conn, handle, data, (uint16_t)len, write_cb, (void *)(uintptr_t)seq) != 0) {
        return -1;
    }
    op_event_t ev;
    wait_t w = wait_op(seq, T_WRITE, &ev);
    if (w == W_TIMEOUT) {
        return connected() ? 1 : -1;
    }
    return w == W_OK && ev.status == 0 ? 0 : -1;
}

/* A write without response; retried while NimBLE is out of buffers. 0 ok, 1 timed out, -1 failed. */
static int write_unacked(session_t *s, uint16_t handle, const uint8_t *data, size_t len)
{
    int64_t until = now_ms() + T_WRITE;
    for (;;) {
        int rc = ble_gattc_write_no_rsp_flat(s->conn, handle, data, (uint16_t)len);
        if (rc == 0) {
            return 0;
        }
        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
            return -1;
        }
        if (now_ms() >= until) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static int subscribe(session_t *s, uint16_t cccd)
{
    static const uint8_t on[2] = {0x01, 0x00};
    return write_acked(s, cccd, on, sizeof(on)) == 0 ? 0 : -1;
}

static int command(session_t *s, const uint8_t *data, size_t len)
{
    /* §3: write-with-response if the characteristic has `write` */
    int rc = s->ctrl_props & BLE_GATT_CHR_PROP_WRITE ? write_acked(s, s->ctrl, data, len)
                                                     : write_unacked(s, s->ctrl, data, len);
    if (rc != 0) {
        return fail(&s->j, rc > 0 ? "write of command %02x timed out" : "write of command %02x failed", data[0]);
    }
    return 0;
}

static void flush_rx(void)
{
    rx_msg_t m;
    while (xQueueReceive(s_rx, &m, 0) == pdTRUE) {
    }
}

typedef bool (*want_fn)(const rx_msg_t *m);

/* The first notification `want` accepts; others are ignored (§4.1). false on timeout. */
static bool wait_rx(want_fn want, int timeout_ms, rx_msg_t *out)
{
    int64_t until = now_ms() + timeout_ms;
    for (;;) {
        TickType_t ticks = ticks_until(until);
        if (!ticks || !connected() || xQueueReceive(s_rx, out, ticks) != pdTRUE) {
            return false;
        }
        if (want(out)) {
            return true;
        }
        ESP_LOGD(TAG, "ignored notification %02x (%u B)", out->data[0], out->len);
    }
}

/* SPEC-GAP: §4 names only `05 00 <index>` and `05 08`; other `05` messages are ignored like any
 * notification we aren't waiting for (§4.1). */
static bool is_block_size(const rx_msg_t *m) { return m->len >= 3 && m->data[0] == CMD_BLOCK_SIZE; }
static bool is_prepare(const rx_msg_t *m) { return m->len >= 1 && m->data[0] == CMD_PREPARE; }
static bool is_request(const rx_msg_t *m) { return m->len >= 6 && m->data[0] == MSG_DATA && m->data[1] == DATA_REQUEST; }
static bool is_done(const rx_msg_t *m) { return m->len >= 2 && m->data[0] == MSG_DATA && m->data[1] == DATA_DONE; }
static bool is_done_or_request(const rx_msg_t *m) { return is_done(m) || is_request(m); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* §4.2: register a `05 00 <index>` request. -1 = abort. */
static int64_t request(session_t *s, const rx_msg_t *m)
{
    uint32_t index = le32(&m->data[2]);
    if (index >= s->count) {
        return fail(&s->j, "tag asked for block %u, there are only %u", (unsigned)index, (unsigned)s->count);
    }
    if (++s->requests[index] > MAX_REQUESTS) {
        return fail(&s->j, "tag asked for block %u more than %d times", (unsigned)index, MAX_REQUESTS);
    }
    return index;
}

static int send_block(session_t *s, uint32_t index)
{
    static uint8_t buf[MAX_BLOCK];
    const ps_pacing_t *pacing = ps_model(s->j.t->sdk_type)->pacing;
    size_t per_block = s->block_size - BLOCK_HEADER;
    size_t offset = (size_t)index * per_block;
    size_t n = s->length - offset < per_block ? s->length - offset : per_block;
    bool last = index == s->count - 1;
    buf[0] = (uint8_t)index, buf[1] = (uint8_t)(index >> 8), buf[2] = (uint8_t)(index >> 16), buf[3] = (uint8_t)(index >> 24);
    ps_image_read(s->j.t->image, s->j.t->framed, offset, buf + BLOCK_HEADER, n);
    bool ack = pacing->ack_every_block || (last && pacing->ack_last_block);
    int rc = ack ? write_acked(s, s->data, buf, BLOCK_HEADER + n) : write_unacked(s, s->data, buf, BLOCK_HEADER + n);
    if (rc < 0) {
        return fail(&s->j, "write of block %u failed", (unsigned)index);
    }
    if (rc > 0) {
        if (!last) {
            return fail(&s->j, "write of block %u timed out", (unsigned)index);
        }
        /* §4.3: on the last block the controller often starts refreshing instead of answering */
        say(&s->j, "no write response for the last block %u, waiting for 05 08", (unsigned)index);
    }
    s->blocks_sent++;
    if (pacing->gap_ms) {
        vTaskDelay(pdMS_TO_TICKS(pacing->gap_ms));
    }
    return 0;
}

/* §4.3: send everything from `index` back to back, follow resend requests. */
static int stream(session_t *s, uint32_t index)
{
    const ps_pacing_t *pacing = ps_model(s->j.t->sdk_type)->pacing;
    say(&s->j, "streaming from block %u (%s pacing)", (unsigned)index, pacing->name);
    for (;;) {
        int64_t restart = -1;
        for (uint32_t i = index; i < s->count; i++) {
            if (s->blocks_sent) {
                rx_msg_t m;
                while (restart < 0 && xQueueReceive(s_rx, &m, 0) == pdTRUE) {
                    if (is_done(&m)) {
                        /* SPEC-GAP: `05 08` before the last block of a stream is taken as success */
                        say(&s->j, "05 08 after block %u: done", (unsigned)(i - 1));
                        return 0;
                    }
                    if (is_request(&m) && (restart = request(s, &m)) < 0) {
                        return -1;
                    }
                }
                if (restart >= 0) {
                    break;
                }
            }
            if (!connected()) {
                return fail(&s->j, "disconnected at block %u", (unsigned)i);
            }
            if (send_block(s, i) != 0) {
                return -1;
            }
        }
        if (restart >= 0) {
            say(&s->j, "tag asks to resend from block %u", (unsigned)restart);
            index = (uint32_t)restart;
            continue;
        }
        rx_msg_t m;
        if (!wait_rx(is_done_or_request, (int)pacing->done_wait_ms, &m)) {
            say(&s->j, "no 05 08 after the last block; counting it as done (§4.2)");
            return 0;
        }
        if (is_done(&m)) {
            say(&s->j, "05 08: tag has the image, refreshing");
            return 0;
        }
        int64_t next = request(s, &m);
        if (next < 0) {
            return -1;
        }
        index = (uint32_t)next;
        say(&s->j, "tag asks to resend from block %u", (unsigned)index);
    }
}

/* §4.4 (unverified): one block per request. */
static int legacy(session_t *s, uint32_t index)
{
    /* SPEC-GAP: §4.4 doesn't say how legacy blocks are written; the type's §4.3 write mode is used */
    say(&s->j, "legacy firmware: one block per request, from block %u", (unsigned)index);
    for (;;) {
        if (send_block(s, index) != 0) {
            return -1;
        }
        bool last = index == s->count - 1;
        rx_msg_t m;
        if (!wait_rx(is_done_or_request, T_LEGACY_NEXT, &m)) {
            if (last) { /* SPEC-GAP: silent after the last block counts as done, as in §4.2 */
                say(&s->j, "no 05 08 after the last block; counting it as done");
                return 0;
            }
            return fail(&s->j, "tag went quiet after block %u", (unsigned)index);
        }
        if (is_done(&m)) {
            if (!last) {
                return fail(&s->j, "05 08 after block %u of %u", (unsigned)index, (unsigned)s->count);
            }
            say(&s->j, "05 08: tag has the image, refreshing");
            return 0;
        }
        int64_t next = request(s, &m);
        if (next < 0) {
            return -1;
        }
        index = (uint32_t)next;
    }
}

/* §4.1 */
static int transfer(session_t *s)
{
    rx_msg_t m;
    bool got = false;
    for (int attempt = 0; attempt < BLOCK_SIZE_TRIES && !got; attempt++) {
        const uint8_t cmd = CMD_BLOCK_SIZE;
        if (command(s, &cmd, 1) != 0) {
            return -1;
        }
        got = wait_rx(is_block_size, T_BLOCK_SIZE, &m);
        if (!got) {
            say(&s->j, attempt + 1 < BLOCK_SIZE_TRIES ? "no block size answer, asking again" : "no block size answer");
        }
    }
    if (!got) {
        return fail(&s->j, "tag didn't tell its block size");
    }
    s->block_size = (uint16_t)(m.data[1] | m.data[2] << 8);
    if (s->block_size <= BLOCK_HEADER || s->block_size > MAX_BLOCK) {
        return fail(&s->j, "unusable block size %u", s->block_size);
    }
    s->length = ps_image_payload_len(s->j.t->image, s->j.t->framed);
    size_t per_block = s->block_size - BLOCK_HEADER;
    s->count = (uint32_t)((s->length + per_block - 1) / per_block);
    say(&s->j, "block size %u: %u bytes in %u blocks", s->block_size, (unsigned)s->length, (unsigned)s->count);
    s->requests = calloc(s->count, 1);
    if (!s->requests) {
        return fail(&s->j, "out of memory");
    }

    /* `02 <u32 length> 01 00 00`: exactly 8 bytes, a 6-byte version never refreshes the panel */
    uint32_t len = (uint32_t)s->length;
    const uint8_t prepare[8] = {CMD_PREPARE, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16),
                                (uint8_t)(len >> 24), 0x01, 0x00, 0x00};
    if (command(s, prepare, sizeof(prepare)) != 0) {
        return -1;
    }
    if (!wait_rx(is_prepare, T_PREPARE, &m)) {
        return fail(&s->j, "no answer to the prepare command");
    }
    /* SPEC-GAP: a bare `02` is taken as accepted; §4.1 only rejects a non-zero second byte */
    if (m.len >= 2 && m.data[1] != 0) {
        return fail(&s->j, "tag rejected the transfer (02 %02x)", m.data[1]);
    }

    const uint8_t start = CMD_START;
    if (command(s, &start, 1) != 0) {
        return -1;
    }
    if (!wait_rx(is_request, T_FIRST_REQUEST, &m)) {
        return fail(&s->j, "tag didn't ask for data after start");
    }
    int64_t index = request(s, &m);
    if (index < 0) {
        return -1;
    }
    return s->j.t->sw & PS_STREAMING_FLAG ? stream(s, (uint32_t)index) : legacy(s, (uint32_t)index);
}

static void disconnect(session_t *s)
{
    if (!connected()) {
        return;
    }
    int rc = ble_gap_terminate(s->conn, BLE_ERR_REM_USER_CONN_TERM);
    op_event_t ev;
    if (rc != 0 || wait_op(s_conn_seq, T_DISCONNECT, &ev) != W_OK) { /* §4.5: logged, not raised */
        say(&s->j, "disconnect: %d", rc ? rc : -1);
    }
}

int psb_send(const psb_transfer_t *t, char *err, size_t err_size)
{
    session_t s = {.j = {.t = t, .err = err, .err_size = err_size}, .conn = BLE_HS_CONN_HANDLE_NONE};
    err[0] = 0;
    const ps_model_t *m = ps_model(t->sdk_type);
    if (!m || !t->image || ps_image_sdk(t->image) != t->sdk_type) {
        return fail(&s.j, "unsupported panel type %d", t->sdk_type);
    }
    if (!s_synced) {
        return fail(&s.j, "Bluetooth not ready");
    }
    s_busy = true;
    s_deadline = now_ms() + T_WHOLE;
    scan_stop();
    xQueueReset(s_ops);
    flush_rx();

    int rc = connect(&s);
    if (rc == 0) {
        exchange_mtu(&s);
        rc = discover(&s);
    }
    if (rc == 0 && subscribe(&s, s.ctrl_cccd) != 0) {
        rc = fail(&s.j, "can't subscribe to the control characteristic");
    }
    if (rc == 0 && (s.data_props & BLE_GATT_CHR_PROP_NOTIFY) && subscribe(&s, s.data_cccd) != 0) {
        say(&s.j, "no notifications on the data characteristic"); /* FEF1 alone is enough (§3) */
    }
    if (rc == 0) {
        vTaskDelay(pdMS_TO_TICKS(T_SETTLE));
        if (t->framed) {
            say(&s.j, "framed for a tag without the raw flag: %u bytes", (unsigned)ps_image_payload_len(t->image, true));
        }
        rc = transfer(&s);
        if (rc != 0 && !err[0]) {
            fail(&s.j, "transfer failed");
        }
        if (rc != 0 && now_ms() >= s_deadline) {
            fail(&s.j, "transfer took longer than %d s", T_WHOLE / 1000);
        }
    }
    s_deadline = now_ms() + T_DISCONNECT + 1000; /* the disconnect gets its own time */
    disconnect(&s);
    free(s.requests);
    s_busy = false;
    if (s_synced) {
        scan_start(BLE_HS_FOREVER);
    }
    return rc == 0 ? 0 : -1;
}
