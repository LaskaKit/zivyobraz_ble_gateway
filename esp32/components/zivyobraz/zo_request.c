/* Request body and answer headers (python/zivyobraz_ble_gateway/zivyobraz.py build_request / parse_answer). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zivyobraz.h"

int zo_build_request(char *out, size_t size, const char *mac, int sdk_type, int battery_dv, int rssi)
{
    const ps_model_t *m = ps_model(sdk_type);
    int w = m ? m->width : 296, h = m ? m->height : 128;
    char system[64] = "{\"resetReason\":\"deepsleep\"";
    if (battery_dv > 0) {
        snprintf(system + strlen(system), sizeof(system) - strlen(system), ",\"vccVoltage\":%d.%d",
                 battery_dv / 10, battery_dv % 10);
    }
    strcat(system, "}");
    char network[80];
    int n = snprintf(network, sizeof(network), "{\"mac\":\"%s\",\"ssid\":\"BLE\"", mac);
    if (rssi != 0) {
        n += snprintf(network + n, sizeof(network) - (size_t)n, ",\"rssi\":%d", rssi);
    }
    snprintf(network + n, sizeof(network) - (size_t)n, "}");
    return snprintf(out, size,
                    "{\"apiVersion\":\"" ZO_API_VERSION "\",\"fwVersion\":\"" ZO_API_VERSION "\","
                    "\"board\":\"" ZO_BOARD "\",\"system\":%s,\"network\":%s,"
                    "\"display\":{\"type\":\"Picksmart-%d\",\"width\":%d,\"height\":%d,\"colorType\":\"%s\"}}",
                    system, network, sdk_type, w, h, ps_color_type(sdk_type));
}

void zo_api_key(uint32_t random, char out[9])
{
    snprintf(out, 9, "%08lu", (unsigned long)(10000000u + random % 90000000u));
}

void zo_answer_init(zo_answer_t *a)
{
    memset(a, 0, sizeof(*a));
    a->sleep = -1;
    a->sleep_minutes = -1;
}

static bool same(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) {
            return false;
        }
    }
    return *a == *b;
}

static bool integer(const char *s, int *out)
{
    char *end;
    while (isspace((unsigned char)*s)) {
        s++;
    }
    long v = strtol(s, &end, 10);
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (end == s || *end) {
        return false;
    }
    *out = (int)v;
    return true;
}

void zo_answer_header(zo_answer_t *a, const char *name, const char *value)
{
    if (same(name, "Timestamp")) {
        while (isspace((unsigned char)*value)) {
            value++;
        }
        snprintf(a->timestamp, sizeof(a->timestamp), "%s", value);
        size_t n = strlen(a->timestamp);
        while (n && isspace((unsigned char)a->timestamp[n - 1])) {
            a->timestamp[--n] = 0;
        }
    } else if (same(name, "PreciseSleep")) {
        if (!integer(value, &a->sleep)) {
            a->sleep = -1;
        }
    } else if (same(name, "Sleep")) {
        if (!integer(value, &a->sleep_minutes)) {
            a->sleep_minutes = -1;
        }
    } else if (same(name, "Rotate")) {
        a->rotate = true;
    }
}

int zo_answer_sleep(const zo_answer_t *a)
{
    if (a->sleep >= 0) {
        return a->sleep;
    }
    return a->sleep_minutes >= 0 ? a->sleep_minutes * 60 : -1;
}
