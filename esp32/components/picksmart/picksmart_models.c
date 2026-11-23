/*
 * The supported panel types (PROTOCOL.md §2, §4.3), advertisement parsing (§1) and the colour
 * rules (§5.1). Mirrors python/picksmart/{models,advert}.py and imaging.py's nearest colour.
 */
#include <stddef.h>

#include "picksmart.h"

static const ps_pacing_t ACKED = {"acked", true, true, 5, 2000};
static const ps_pacing_t STREAM = {"stream", false, false, 40, 20000};
static const ps_pacing_t FENCED = {"fenced", false, true, 100, 60000};

static const ps_model_t MODELS[] = {
    {46, 296, 128, PS_COLOURS_BWRY, PS_ENC_2BPP_ROTATED, &ACKED, "2.13in 296x128 BWRY"},
    {75, 400, 300, PS_COLOURS_BWR, PS_ENC_PLANES, &STREAM, "4.2in 400x300 BWR"},
    {160, 250, 122, PS_COLOURS_BW, PS_ENC_ROW_PAIRS, &STREAM, "2.9in TFT 250x122 BW"},
    {270, 250, 128, PS_COLOURS_BWRY, PS_ENC_2BPP_ROTATED, &STREAM, "2.9in 250x122 BWRY"},
    {299, 800, 480, PS_COLOURS_BWR, PS_ENC_PLANES_FLIPPED, &FENCED, "7.5in 800x480 BWR"},
};

const ps_model_t *ps_model(int sdk_type)
{
    for (size_t i = 0; i < sizeof(MODELS) / sizeof(MODELS[0]); i++) {
        if (MODELS[i].sdk == sdk_type) {
            return &MODELS[i];
        }
    }
    return NULL;
}

int ps_sdk_from_raw(int raw_type)
{
    int sdk = raw_type & 0x3FFF;
    if (ps_model(sdk)) {
        return sdk;
    }
    if (ps_model(raw_type & 0xFF)) {
        return raw_type & 0xFF;
    }
    return sdk;
}

bool ps_is_bwry(int sdk_type)
{
    const ps_model_t *m = ps_model(sdk_type);
    return m && m->colours == PS_COLOURS_BWRY;
}

bool ps_is_bw(int sdk_type)
{
    const ps_model_t *m = ps_model(sdk_type);
    return m && m->colours == PS_COLOURS_BW;
}

const char *ps_color_type(int sdk_type)
{
    return ps_is_bw(sdk_type) ? "BW" : ps_is_bwry(sdk_type) ? "4C" : "3C";
}

bool ps_parse_mfg(const uint8_t *data, size_t len, ps_adv_t *out)
{
    if (len < 2 + 5 || (data[0] | data[1] << 8) != PS_COMPANY_ID) {
        return false;
    }
    const uint8_t *d = data + 2;
    out->raw_type = (uint16_t)(d[4] << 8 | d[0]);
    out->sdk_type = (uint16_t)ps_sdk_from_raw(out->raw_type);
    out->battery_dv = d[1];
    out->sw = d[2];
    out->hw = d[3];
    return true;
}

ps_color_t ps_panel_color(int sdk_type, ps_color_t c)
{
    const ps_model_t *m = ps_model(sdk_type);
    if (!m) {
        return c;
    }
    if (m->colours == PS_COLOURS_BW && c != PS_WHITE) {
        return PS_BLACK;
    }
    if (m->colours == PS_COLOURS_BWR && c == PS_YELLOW) {
        return PS_RED;
    }
    return c;
}
