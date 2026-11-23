/*
 * PC test of the portable ESP32 code (picksmart, zivyobraz) against vectors made by the
 * Python gateway (gen_vectors.py). Run with esp32/test/host/run.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guard.h"
#include "version.h"
#include "picksmart.h"
#include "zivyobraz.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void test_image(const char *dir, int n, int sdk, int rotate, int framed)
{
    char path[512], what[96];
    size_t body_len, want_len;
    snprintf(path, sizeof(path), "%s/%d.body", dir, n);
    uint8_t *body = slurp(path, &body_len);
    snprintf(path, sizeof(path), "%s/%d.payload", dir, n);
    uint8_t *want = slurp(path, &want_len);
    snprintf(what, sizeof(what), "vector %d (sdk %d, rotate %d, framed %d)", n, sdk, rotate, framed);
    if (!body || !want) {
        check(0, what);
        return;
    }
    ps_image_t *img = ps_image_new(sdk, rotate);
    check(img != NULL, what);
    if (!img) {
        return;
    }
    zo_decoder_t *d = zo_decoder_new(sdk, ps_image_width(img), ps_image_height(img), zo_sink_image, img);
    /* feed in odd-sized pieces, like a network stream */
    size_t at = 0, step = 1;
    while (at < body_len) {
        size_t take = body_len - at < step ? body_len - at : step;
        if (!zo_decoder_feed(d, body + at, take)) {
            printf("  decoder: %s\n", zo_decoder_error(d));
            check(0, what);
            break;
        }
        at += take;
        step = step * 7 % 1500 + 1;
    }
    check(zo_decoder_finish(d), what);
    size_t len = ps_image_payload_len(img, framed);
    uint8_t *got = malloc(len);
    ps_image_read(img, framed, 0, got, len);
    if (len != want_len) {
        printf("  length %zu, want %zu\n", len, want_len);
        check(0, what);
    } else if (memcmp(got, want, len)) {
        size_t i = 0;
        while (got[i] == want[i]) {
            i++;
        }
        printf("  first difference at byte %zu: %02x, want %02x\n", i, got[i], want[i]);
        check(0, what);
    }
    zo_decoder_free(d);
    ps_image_free(img);

    char fpath[512];
    snprintf(fpath, sizeof(fpath), "%s/%d.work", dir, n);
    ps_image_t *fimg = ps_image_new_file(sdk, rotate, fpath);
    snprintf(what, sizeof(what), "file-backed vector %d", n);
    check(fimg != NULL, what);
    if (fimg) {
        zo_decoder_t *fd = zo_decoder_new(sdk, ps_image_width(fimg), ps_image_height(fimg), zo_sink_image, fimg);
        size_t fat = 0;
        while (fat < body_len) {
            size_t take = body_len - fat < 1024 ? body_len - fat : 1024;
            if (!zo_decoder_feed(fd, body + fat, take)) {
                check(0, what);
                break;
            }
            fat += take;
        }
        check(zo_decoder_finish(fd), what);
        uint8_t *fgot = malloc(len);
        ps_image_read(fimg, framed, 0, fgot, len);
        if (memcmp(got, fgot, len)) {
            printf("  file-backed payload differs from RAM\n");
            check(0, what);
        }
        free(fgot);
        zo_decoder_free(fd);
        ps_image_free(fimg);
        remove(fpath);
    }

    free(got);
    free(body);
    free(want);
}

static void test_request(char *line)
{
    char mac[32];
    int sdk, dv, rssi, used = 0;
    sscanf(line, "req %31s %d %d %d %n", mac, &sdk, &dv, &rssi, &used);
    char *want = line + used;
    want[strcspn(want, "\n")] = 0;
    char got[512];
    zo_build_request(got, sizeof(got), mac, sdk, dv, rssi);
    if (strcmp(got, want)) {
        printf("  got  %s\n  want %s\n", got, want);
        check(0, "request JSON");
    }
}

static void test_misc(void)
{
    ps_adv_t a;
    const uint8_t mfg[] = {0x53, 0x50, 0x2E, 0x1E, 0x81, 0x01, 0x40};
    check(ps_parse_mfg(mfg, sizeof(mfg), &a) && a.raw_type == 0x402E && a.sdk_type == 46 && a.battery_dv == 30 &&
              a.sw == 0x81 && a.hw == 1,
          "advert parse");
    const uint8_t other[] = {0x4C, 0x00, 1, 2, 3, 4, 5};
    check(!ps_parse_mfg(other, sizeof(other), &a), "advert: other company");
    check(strcmp(ps_color_type(160), "BW") == 0 && strcmp(ps_color_type(46), "4C") == 0 &&
              strcmp(ps_color_type(75), "3C") == 0,
          "colour types");

    zo_answer_t ans;
    zo_answer_init(&ans);
    zo_answer_header(&ans, "timestamp", " 05242116 ");
    zo_answer_header(&ans, "Sleep", "3");
    check(strcmp(ans.timestamp, "05242116") == 0 && zo_answer_sleep(&ans) == 180 && !ans.rotate, "answer: Sleep");
    zo_answer_header(&ans, "PRECISESLEEP", "95");
    zo_answer_header(&ans, "Rotate", "");
    check(zo_answer_sleep(&ans) == 95 && ans.rotate, "answer: PreciseSleep wins");

    char key[9];
    zo_api_key(0, key);
    check(strcmp(key, "10000000") == 0, "api key low");
    zo_api_key(0xFFFFFFFFu, key);
    check(strlen(key) == 8 && key[0] != '0', "api key high");

    ps_image_t *img = ps_image_new(75, false);
    zo_decoder_t *d = zo_decoder_new(75, 400, 300, zo_sink_image, img);
    check(!zo_decoder_finish(d), "empty body is an error");
    zo_decoder_free(d);
    d = zo_decoder_new(75, 400, 300, zo_sink_image, img);
    check(!zo_decoder_feed(d, (const uint8_t *)"<html>", 6), "html body is an error");
    zo_decoder_free(d);
    ps_image_free(img);
    check(ps_image_new(12345, false) == NULL, "unknown type refused");
    check(ps_image_new(78, false) == NULL, "only the PROTOCOL.md §2 types");
    check(ps_sdk_from_raw(0x204B) == 75 && ps_sdk_from_raw(0x412B) == 299 && ps_sdk_from_raw(0x404E) == 78,
          "sdk type from raw type");
    img = ps_image_new(75, false);
    d = zo_decoder_new(75, 400, 300, zo_sink_image, img);
    check(!zo_decoder_feed(d, (const uint8_t *)"\x89PNG\r\n", 6), "PNG body is an error (Z2/Z3 only)");
    zo_decoder_free(d);
    d = zo_decoder_new(75, 400, 300, zo_sink_image, img);
    check(!zo_decoder_feed(d, (const uint8_t *)"Z1\x01\x03", 4), "Z1 body is an error (no longer used)");
    zo_decoder_free(d);
    ps_image_free(img);
    img = ps_image_new(299, false);
    check(img && ps_image_payload_len(img, false) == 96000 && ps_image_payload_len(img, true) == 100504,
          "SDK 299 payload lengths");
    ps_image_free(img);
}

/* Same cases as python/tests/test_gateway.py Guard. */
static void test_guard(void)
{
    static const struct {
        const char *host;
        bool ok;
    } hosts[] = {
        {"zo-ble-gateway.local", true}, {"ZO-BLE-Gateway.local:80", true}, {"zo-ble-gateway", true},
        {"zo-ble-gateway.lan", true}, {"zo-ble-gateway.home.arpa", true}, {"zo-ble-gateway.local.", true},
        {"192.168.1.20", true}, {"192.168.1.20:8080", true}, {"[fe80::1]", true}, {"[fe80::1]:80", true},
        {"localhost:8080", true},
        {"evil.example", false}, {"zo-ble-gateway.evil.example", false}, {"zo-ble-gateway.local.evil.example", false},
        {"zo-ble-gatewayx.local", false}, {"192.168.1", false}, {"1.2.3.4.example", false}, {"1234.1.1.1", false},
        {"zo-ble-gateway.local:8o", false}, {"", false}, {":80", false},
    };
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        if (guard_host_ok(hosts[i].host, "zo-ble-gateway") != hosts[i].ok) {
            printf("  host '%s'\n", hosts[i].host);
            check(0, "guard_host_ok");
        }
    }
    check(!guard_host_ok(NULL, "zo-ble-gateway"), "no Host header");

    const char *h = "zo-ble-gateway.local";
    check(guard_origin_ok(NULL, h) && guard_origin_ok("", h), "no Origin: not a browser");
    check(guard_origin_ok("http://zo-ble-gateway.local", h), "same origin");
    check(guard_origin_ok("http://ZO-ble-gateway.local/settings?x", h), "Referer of our page");
    check(!guard_origin_ok("http://evil.example", h), "other site");
    check(!guard_origin_ok("http://zo-ble-gateway.local.evil.example", h), "other site, our name as prefix");
    check(!guard_origin_ok("http://zo-ble-gateway.local:8080", h), "other port");
    check(!guard_origin_ok("null", h), "opaque origin");
    check(guard_origin_ok("http://192.168.1.20:8080", "192.168.1.20:8080"), "IP and port");

    check(guard_valid_mac("FF:ff:92:85:05:77"), "valid mac");
    check(!guard_valid_mac("+F:FF:92:85:05:77") && !guard_valid_mac(" F:FF:92:85:05:77") &&
              !guard_valid_mac("FF:FF:92:85:05:7") && !guard_valid_mac("FF-FF-92-85-05-77") && !guard_valid_mac(NULL),
          "invalid macs");
    check(guard_valid_key("48151623"), "valid key");
    check(!guard_valid_key("4815162") && !guard_valid_key("481516234") && !guard_valid_key("<b>x</b>") &&
              !guard_valid_key("4815\r\n23") && !guard_valid_key(NULL),
          "invalid keys");
}

static void test_version(void)
{
    check(version_cmp("0.3.0", "0.2.0") > 0 && version_cmp("0.2.0", "0.3.0") < 0, "version order");
    check(version_cmp("0.10.0", "0.9.9") > 0, "version: numbers, not text");
    check(version_cmp("v1.2", "1.2.0") == 0 && version_cmp("1.2.0", "1.2") == 0, "version: v and missing parts");
    check(version_cmp("1.0.0-rc1", "1.0.0") < 0 && version_cmp("1.0.0", "1.0.0-rc1") > 0, "version: rc is older");
    check(version_cmp("1.0.1-rc1", "1.0.0") > 0, "version: rc of a newer one");
    check(version_cmp("0.2.0-3-gabc-dirty", "0.2.0") < 0, "version: git describe of a dev build");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s VECTOR_DIR\n", argv[0]);
        return 2;
    }
    char path[512], line[1024];
    snprintf(path, sizeof(path), "%s/manifest.txt", argv[1]);
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 2;
    }
    int images = 0, requests = 0;
    while (fgets(line, sizeof(line), f)) {
        int n, sdk, rotate, framed;
        if (sscanf(line, "img %d %d %d %d", &n, &sdk, &rotate, &framed) == 4) {
            test_image(argv[1], n, sdk, rotate, framed);
            images++;
        } else if (strncmp(line, "req ", 4) == 0) {
            test_request(line);
            requests++;
        }
    }
    fclose(f);
    test_misc();
    test_guard();
    test_version();
    printf("%d image vectors, %d requests, %s\n", images, requests, failures ? "FAILED" : "all OK");
    return failures ? 1 : 0;
}
