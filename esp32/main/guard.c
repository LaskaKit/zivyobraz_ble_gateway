/* See guard.h. */
#include "guard.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static bool ip_literal(const char *h, size_t n)
{
    if (n >= 2 && h[0] == '[' && h[n - 1] == ']') { /* IPv6 */
        for (size_t i = 1; i + 1 < n; i++) {
            if (!isxdigit((unsigned char)h[i]) && h[i] != ':' && h[i] != '.') {
                return false;
            }
        }
        return n > 2;
    }
    int dots = 0, digits = 0;
    for (size_t i = 0; i < n; i++) {
        if (h[i] == '.') {
            if (!digits) {
                return false;
            }
            dots++, digits = 0;
        } else if (!isdigit((unsigned char)h[i]) || ++digits > 3) {
            return false;
        }
    }
    return dots == 3 && digits;
}

bool guard_host_ok(const char *host, const char *hostname)
{
    if (!host) {
        return false;
    }
    size_t n = strlen(host);
    /* drop ":port" (but not the colons inside "[v6]") */
    const char *colon = strrchr(host, ':');
    if (colon && (host[0] != '[' || (colon > host && colon[-1] == ']'))) {
        for (const char *p = colon + 1; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                return false;
            }
        }
        n = (size_t)(colon - host);
    }
    if (n && host[n - 1] == '.') { /* "name.local." */
        n--;
    }
    if (!n) {
        return false;
    }
    if (ip_literal(host, n) || (n == 9 && strncasecmp(host, "localhost", 9) == 0)) {
        return true;
    }
    size_t hn = strlen(hostname);
    if (!hn || n < hn || strncasecmp(host, hostname, hn) != 0) {
        return false;
    }
    if (n == hn) {
        return true;
    }
    if (host[hn] != '.') {
        return false;
    }
    static const char *suffixes[] = {"local", "lan", "home", "home.arpa", "localdomain", "internal"};
    const char *rest = host + hn + 1;
    size_t rn = n - hn - 1;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (rn == strlen(suffixes[i]) && strncasecmp(rest, suffixes[i], rn) == 0) {
            return true;
        }
    }
    return false;
}

bool guard_origin_ok(const char *origin, const char *host)
{
    if (!origin || !origin[0]) {
        return true; /* not from a browser (browsers send Origin on every POST) */
    }
    if (!host) {
        return false;
    }
    const char *p = strncasecmp(origin, "http://", 7) == 0 ? origin + 7
                    : strncasecmp(origin, "https://", 8) == 0 ? origin + 8
                                                              : NULL;
    if (!p) {
        return false; /* "null" (sandboxed frames, file://) and anything odd */
    }
    size_t n = strcspn(p, "/?#"); /* Referer has a path */
    return n == strlen(host) && strncasecmp(p, host, n) == 0;
}

bool guard_valid_mac(const char *mac)
{
    if (!mac || strlen(mac) != 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? mac[i] != ':' : !isxdigit((unsigned char)mac[i])) {
            return false;
        }
    }
    return true;
}

bool guard_valid_key(const char *key)
{
    if (!key || strlen(key) != 8) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)key[i])) {
            return false;
        }
    }
    return true;
}
