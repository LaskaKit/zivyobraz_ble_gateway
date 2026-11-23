/*
 * Request checks for the web page and the tags.json import. Plain C without ESP-IDF, so
 * test/host tests it on the PC. The Python gateway does the same in web.py (`host_ok`, `origin_ok`).
 *
 * Host: the page answers only to names an attacker's DNS can't point at us (DNS rebinding):
 * an IP literal, "localhost", our own host name alone or with a LAN-only suffix (.local, .lan,
 * .home, .home.arpa, .localdomain, .internal). An optional ":port" is ignored.
 * Origin (or Referer) of a POST: missing (curl), or http(s)://<the Host header> (cross-site forms
 * and fetch() carry the attacker's origin, so they are refused).
 */
#pragma once

#include <stdbool.h>

bool guard_host_ok(const char *host, const char *hostname);
bool guard_origin_ok(const char *origin, const char *host);

/* "FF:FF:92:85:05:77": six pairs of hex digits (any case) with colons */
bool guard_valid_mac(const char *mac);
/* the zivyobraz key: exactly 8 decimal digits */
bool guard_valid_key(const char *key);
