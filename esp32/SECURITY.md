# ESP32 firmware: security review and OTA options

Review of `esp32/` as of 0.2.0 (2026-11-23). Fixed since then: #1, #2, #4, #5, #6 (marked **fixed** below; the
Python gateway got the same Host/Origin guard). OTA options A and B are implemented (§3). The rest is still open.
Threat model assumed: a home LAN gateway on Ethernet, with tags in BLE range.

## 1. What is already fine

* **HTTPS to zivyobraz.eu** is verified against the ESP-IDF certificate bundle (`esp_crt_bundle_attach`)
  and the host name is checked. Nothing on the request side is built from untrusted input.
* **Server body parsing** (`zo_image.c`) streams, keeps every run inside `width × height`, and caps the body at
  2 MB. Header values are copied with `snprintf` into fixed buffers.
* **BLE parsing** (`picksmart_ble.c`, `ps_parse_mfg`) checks lengths everywhere: AD fields, notification length
  (copied into a 23-byte buffer), block index `< count`, block size `≤ 512`, request count per block.
  A malicious peripheral that impersonates a tag can't overflow anything.
* **Web output**: server-supplied text (Timestamp, poll/push results, log) goes through `esc()`. The preview
  URL is parsed with a strict `%12[0-9A-F]` and mapped to a fixed path, so there is no path traversal.
* **OTA integrity**: `esp_ota_end()` checks the image SHA-256, the project name is compared, and
  `BOOTLOADER_APP_ROLLBACK_ENABLE` together with the 60 s `confirm_firmware` gives a safe fallback.
* **Wi-Fi is never started**, which shrinks the radio attack surface to BLE central/observer only.
* The API key comes from `esp_random()` while the BT radio is on, so the RNG is the hardware TRNG.

## 2. Findings, most important first

| # | Issue | Where | Impact |
|---|---|---|---|
| 1 | **fixed** · **Cross-site request forgery on every POST, firmware update included** | `web.c` (`/update`, `/import`, `/add`, `/remove`, `/settings`) | Any web page the user opens can `fetch('http://zo-ble-gateway.local/update', {method:'POST', mode:'no-cors', body: blob})`. A blob without a type is a "simple" request, so the browser sends it without a CORS preflight. The project-name check is trivial to satisfy, so this replaces the firmware from the internet with no click. Chrome's Local Network Access prompt blocks some cases; Firefox and Safari don't. |
| 2 | **fixed** · **No Host header check, so DNS rebinding works** | `web.c` | A rebinding page can *read* `/tags.json` (every zivyobraz key) and drive every endpoint as same-origin. |
| 3 | **No authentication** | `web.c` | Anyone on the LAN can replace the firmware, download the keys, or remove tags. This is documented ("home LAN"), but together with #1 and #2 the attacker no longer has to be on the LAN. |
| 4 | **fixed** (option A) · **OTA image isn't signed** | `update_post` | The SHA-256 only catches corruption. Any ESP-IDF app with `project_name = zivyobraz-ble-gateway` is accepted, and so is a downgrade. |
| 5 | **fixed** · **Single-threaded httpd can be stalled forever** | `read_body`, `update_post` | `HTTPD_SOCK_ERR_TIMEOUT` → `continue` with no limit. One LAN client that sends `Content-Length: 1000000` and then nothing blocks the web page until it disconnects. |
| 6 | **fixed** · **Imported `api_key` isn't validated** | `tag_from_json` → `tag_card` (`putf("%s")`), `X-API-Key` header | Only its length is checked (8). The key is printed into HTML unescaped and sent as a header, so a crafted `tags.json` can inject markup or CR/LF. Eight characters leave little room for an exploit, but it's an easy fix: require 8 digits. `e->mac` is also printed unescaped; `valid_mac` lets `+`/space through `%2x`, which is harmless but loose. |
| 7 | **Secrets in plaintext flash, no secure boot** | `partitions.csv`, sdkconfig | With physical USB access, `esptool read-flash` gives `tags.json` (the keys), and any firmware can be flashed. That's normal for a hobby device; it only matters if the gateway is installed somewhere untrusted. |
| 8 | **TLS doesn't check certificate dates** | `MBEDTLS_HAVE_TIME_DATE` is off | An expired or not-yet-valid certificate is accepted. Low risk, because the chain and the host name are still verified. SNTP is already running, so this can be turned on. |
| 9 | **BLE is unauthenticated** (protocol limitation) | Picksmart §3 | Anything that advertises a registered tag's MAC receives that tag's picture, and fake adverts can fill the 48-entry "in range" list and the log. This can't be fixed on the gateway side without changing the tags. |

### Recommended fixes (cheap → expensive)

1. **Done (#1, #2):** `main/guard.c`, called by `guarded()` in `web.c` before every handler. The `Host` must be an
   IP literal, `localhost`, or `zo-ble-gateway` alone or with a LAN-only suffix (`.local`, `.lan`, `.home`,
   `.home.arpa`, `.localdomain`, `.internal`). A POST's `Origin` (else `Referer`) must be missing or
   `http(s)://<Host>`. `/update` and `/import` also need `X-ZO-Upload`, which the page's `fetch()` sends. A
   cross-site request with a custom header needs a CORS preflight, and the gateway doesn't answer
   preflights. Refusals get `403` and a log line. The Python `web.py` has the same rules (`host_ok`,
   `origin_ok`, plus `--allow-host`). The test cases are shared by `test_host.c` and `tests/test_gateway.py`.
2. **Done (#6):** `guard_valid_mac` (strict hex pairs) and `guard_valid_key` (8 digits) on import. The MAC and
   key are escaped on the page.
3. **Done (#5):** a receive gives up after 3 timeouts in a row (about 15 s idle). A form body larger than its
   buffer is refused rather than truncated.
4. **Optional password (#3):** an admin password in NVS, set on first visit, sent as HTTP Basic auth or as a
   session cookie (`SameSite=Strict; HttpOnly`) that protects `/settings`, `/update`, `/import` and
   `/tags.json`. Without TLS on the LAN, Basic auth can be sniffed, but it still stops drive-by attacks and
   the neighbour's kid. Make it a `menuconfig` option off by default, if that matches the home-LAN goal.
5. **Done (#4):** signed app images, §3 option A. Downgrades are still possible with a signed older release;
   the GitHub update only offers newer versions, but an upload doesn't check.
6. Turn on `CONFIG_MBEDTLS_HAVE_TIME_DATE` (#8). Because the clock comes from SNTP, a poll before NTP has
   answered must either wait or skip the date check.
7. **Hardening for exposed installations (#7):** secure boot v2 plus flash encryption, see §3, option C.

Smaller build hardening, with almost no cost:
`CONFIG_COMPILER_STACK_CHECK_MODE_NORM` (stack canaries; the code is about 1 kB larger),
`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` (crash forensics; needs a coredump partition, i.e. a USB reflash),
and `CONFIG_BOOTLOADER_LOG_LEVEL_WARN` (less information on UART).

## 3. OTA: current state and options

**Today:** a browser upload to `/update` → `esp_ota_begin/write/end` into the other 3 MB slot →
`set_boot_partition` → reboot. Rollback happens if the new app doesn't run for 60 s. A partition-table change
needs a USB reflash. CI (`.github/workflows/esp32.yml`) builds `-ota.bin` and attaches it to the GitHub release
for each `v*` tag.

| Option | What it is | Effort | Security gain | Risk / cost |
|---|---|---|---|---|
| **A. Signed app images, no secure boot** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._ON_UPDATE_NO_SECURE_BOOT`) | The build appends a signature; `esp_ota_end()` refuses an image that isn't signed with your key. No eFuses are burned. | Small: Kconfig, keeping the private key out of git, and a signing step in `release.sh` and CI (GitHub secret). | High: stops malicious or CSRF uploads (#1, #4). Doesn't stop USB flashing. | Reversible. The ESP32 needs chip rev ≥ 3.0 for RSA (scheme v2), which WROOM-32E is; set `ESP32_REV_MIN_3`. **Losing the key means updates are USB-only.** Verification happens in the *running* app (`esp_ota_end`), so the first signed build can still arrive by the current, unverified web upload. A USB flash is needed only for the optional check at boot (`..._ON_BOOT_...`), because that check lives in the bootloader. |
| **B. Pull OTA from GitHub releases** (`esp_https_ota`) | A button such as "Check for update", or a daily check: fetch the latest release JSON, compare with `version.txt`, then download `-ota.bin` over HTTPS (the certificate bundle already includes GitHub's CA). | Medium: about 150 lines; `esp_https_ota` handles streaming and the slot. Release redirects go to `objects.githubusercontent.com`, which needs `max_redirection_count`. | Only good **together with A**: TLS proves the file came from GitHub, not that *you* built it. | It needs RAM for TLS while BLE runs, so run it between polls like `zo_fetch` (it fits, because `zo_fetch` already does TLS with no PSRAM). Unattended auto-update should stay opt-in. |
| **C. Secure boot v2 + flash encryption (release mode)** | eFuse-anchored: the ROM only boots signed bootloaders and apps, and flash content (keys, `tags.json`) is unreadable off-chip. | Medium–large: one-way eFuse burning, encrypted `storage`/`nvs` partitions, and a partition-table change (USB). | Highest: also covers physical attackers (#7). | **Irreversible.** A bricked board on a mistake, and no plain USB reflashing afterwards (encrypted images only). It conflicts with the "flash it yourself" story for users. Only worth it for installations in untrusted places, and per board, not by default. |
| **D. Anti-rollback** (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, `secure_version` in eFuse) | Refuses older images after an upgrade. | Small, once C exists. | Stops downgrade to a known-vulnerable build. | Needs secure boot to mean anything, and burns eFuse bits per version step (limited count). A software-only alternative with A: refuse `new_app->version < running` in `update_post`, plus a "force" checkbox. |
| **E. Delta / compressed OTA** (`espressif/esp_delta_ota`) | Sends a patch instead of the full app. | Medium. | None. | Not worth it: Ethernet, a 3 MB slot, and an app of about 0.9 MB. |

**Implemented (2026-11-23): A + B, notify and install on click.** See the [README](README.md#updates).
* A: `sdkconfig.defaults` (`SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA, `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`,
  `ESP32_REV_MIN_3`, build unsigned). `release.sh` signs with `signing_key.pem` / `$ZO_SIGNING_KEY` and verifies
  the signature. CI gets the key from the `ZO_SIGNING_KEY` secret, on tag builds only. The trusted key is
  the one in the *running* app's signature block (ESP-IDF `secure_boot_signatures_app.c`), so an unsigned
  development build refuses every update (`update.c` reports this on the settings page).
* B: `main/update.c`. `latest.json` comes from `releases/latest/download/`, which is not GitHub's API, so there's
  no rate limit; the binary comes from `releases/download/<tag>/<file>` through `esp_https_ota`. The project
  name and version in the image header are checked before anything is written. It runs in the gateway task
  between polls, so TLS never overlaps a BLE transfer or a zivyobraz fetch. GitHub's redirect URL is
  about 1 kB, hence the 2 kB TX buffer.
* Not tested on a board yet (2026-11-23): first install of a signed build, a check against a real release, an
  install, and a refusal of a wrongly signed image.

**Original recommendation:** do A first. It's cheap, reversible, and turns the web upload from "anyone can flash" into
"only your releases". Add the CSRF and Host guard from §2 at the same time. B is a nice convenience
afterwards, and it's safe only because of A. Keep C as a documented, opt-in recipe for exposed sites, never as
a default.
