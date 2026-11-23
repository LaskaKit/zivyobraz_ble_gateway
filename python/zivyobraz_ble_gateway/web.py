"""The web page: registered tags, tags in range (with Add), recent log.

Plain HTML forms that POST and redirect back to `/` (no JavaScript apart from a confirm()),
so the same page can later be served by the ESP32-S3. The page reloads itself every 15 s.
The look (style.css) is shared with the ESP32 gateway. No password: it's for a home LAN, but every
request passes `guard` (same rules as the ESP32's guard.c): only our own host names (against DNS
rebinding) and POSTs only from this page (against cross-site forms and fetch()).
"""

from __future__ import annotations

import collections
import html
import ipaddress
import logging
import re
import socket
import time
from pathlib import Path

from picksmart.models import BW_TYPES, BWRY_TYPES

from . import zivyobraz
from .service import Gateway, preview_name

log = logging.getLogger("zivyobraz_ble_gateway")
WWW = Path(__file__).resolve().parents[2] / "web"  # shared with the ESP32 gateway
LOG_LINES = 40

BLACK, WHITE, RED, YELLOW = "#111", "#fff", "#d7263d", "#f4c20d"
SCHEMES = {
    "BW": ("Black / white", [BLACK, WHITE]),
    "3C": ("Black / white / red", [BLACK, WHITE, RED]),
    "4C": ("Black / white / red / yellow", [BLACK, WHITE, RED, YELLOW]),
}


class LogRing(logging.Handler):
    """The last LOG_LINES log lines ("HH:MM:SS text") for the page."""

    def __init__(self) -> None:
        super().__init__(logging.INFO)
        self.lines: collections.deque[str] = collections.deque(maxlen=LOG_LINES)

    def emit(self, record: logging.LogRecord) -> None:
        self.lines.append(time.strftime("%H:%M:%S ", time.localtime(record.created)) + record.getMessage())


LAN_SUFFIXES = ("local", "lan", "home", "home.arpa", "localdomain", "internal")


def host_ok(host: str | None, names) -> bool:
    """A Host header that DNS rebinding can't produce: an IP literal, "localhost", or one of `names`
    alone or with a LAN-only suffix (.local, .lan, ...). An optional ":port" is ignored."""
    if not host:
        return False
    m = re.fullmatch(r"(\[[0-9A-Fa-f:.]+\]|[^:\[\]]*)(?::(\d+))?", host)
    if not m:
        return False
    name = m.group(1).lower().removesuffix(".")
    if not name:
        return False
    try:
        ipaddress.ip_address(name.strip("[]"))
        return True
    except ValueError:
        pass
    if name == "localhost":
        return True
    return any(name == n or name in (f"{n}.{s}" for s in LAN_SUFFIXES) for n in (x.lower() for x in names if x))


def origin_ok(origin: str | None, host: str) -> bool:
    """Origin (or Referer) of a POST: missing (not a browser) or http(s)://<the Host header>."""
    if not origin:
        return True
    m = re.match(r"https?://([^/?#]*)", origin, re.IGNORECASE)
    return bool(m) and m.group(1).lower() == host.lower()


def own_names() -> set[str]:
    """This machine's names as they may appear in the Host header."""
    names = {socket.gethostname(), socket.gethostname().split(".")[0]}
    try:
        names.add(socket.getfqdn())
    except OSError:
        pass
    return {n.lower() for n in names if n}


def esc(text) -> str:
    return html.escape(str(text))


def when(t: float | None) -> str:
    """Epoch -> "20.11. 14:05" (today: just "14:05"), None -> "never"."""
    if not t:
        return "never"
    if time.strftime("%Y%m%d", time.localtime(t)) == time.strftime("%Y%m%d"):
        return time.strftime("%H:%M:%S", time.localtime(t))
    return time.strftime("%d.%m. %H:%M", time.localtime(t))


def duration(seconds: float) -> str:
    seconds = max(0, int(seconds))
    if seconds < 90:
        return f"{seconds} s"
    if seconds < 5400:
        return f"{round(seconds / 60)} min"
    return f"{seconds / 3600:.1f} h"


def inches(model: str) -> str | None:
    """'4.2in 400x300 BWR' -> '4.2'. None if the model name has no size."""
    m = re.match(r"(\d+(?:\.\d+)?)in\b", model)
    return m.group(1) if m else None


def swatches(sdk_type: int) -> str:
    name, colours = SCHEMES[zivyobraz.color_type(sdk_type)]
    return "<span class=sw>" + "".join(f"<i style='background:{c}'></i>" for c in colours) + f"</span>{name}"


def status(text: str | None) -> str:
    if not text:
        return "<span class='st none'>–</span>"
    cls = "ok" if text.startswith("ok") else "bad"
    return f"<span class='st {cls}'>{esc(text)}</span>"


def battery(dv: int | None) -> str:
    return f"<span class=batt>{dv / 10:.1f} V</span>" if dv else "<span class='batt none'>–</span>"


def head(sdk_type: int) -> str:
    model = zivyobraz.model_text(sdk_type)
    w, h = zivyobraz.display_size(sdk_type)
    size = inches(model)
    out = "<div class=spec>"
    if size:
        out += f"<div class=inch>{size}<span>&Prime;</span></div>"
    kind = "BW" if sdk_type in BW_TYPES else "BWRY" if sdk_type in BWRY_TYPES else "BWR"
    tft = " TFT" if "TFT" in model else ""
    return out + (f"<div class=specs><b class=panel>Picksmart {kind}{tft}</b>"
                  f"<span>{w} &times; {h} px · type {sdk_type}</span></div></div>")


def tag_card(gw: Gateway, mac: str, e: dict) -> str:
    now = time.time()
    sdk_type = e["sdk_type"]
    in_range = mac in gw.seen
    if gw.busy == mac:
        state = "<span class='st ok'>sending…</span>"
    elif mac in gw.pending:
        state = "<span class='st none'>waiting to send</span>"
    else:
        state = status(e.get("push_result"))
    preview = gw.preview_dir / preview_name(mac)
    img = (f"<img class=preview src='/preview/{preview_name(mac)}?v={int(preview.stat().st_mtime)}' alt=''>"
           if preview.exists() else "")
    return (
        f"<li>{head(sdk_type)}<div class=mac>{esc(mac)}</div>{img}"
        f"<div class=key>zivyobraz key <b>{esc(e['api_key'])}</b></div><dl>"
        f"<dt>Colours</dt><dd>{swatches(sdk_type)}</dd>"
        f"<dt>Battery</dt><dd>{battery(e.get('battery_dv'))}</dd>"
        f"<dt>Signal</dt><dd>{esc(e.get('rssi', '–')) if in_range else 'not in range'}"
        f"{' dBm' if in_range and e.get('rssi') is not None else ''}</dd>"
        f"<dt>Last seen</dt><dd>{when(e.get('last_seen'))}</dd>"
        f"<dt>Server</dt><dd>{status(e.get('poll_result'))}</dd>"
        f"<dt>Asked</dt><dd>{when(e.get('last_poll'))}</dd>"
        f"<dt>Next</dt><dd>in {duration(e.get('next_poll', 0) - now)}</dd>"
        f"<dt>Transfer</dt><dd>{state}</dd>"
        f"<dt>Sent</dt><dd>{when(e.get('last_push'))}</dd>"
        f"<dt>Content</dt><dd>{esc(e.get('timestamp') or '–')}{' · rotated' if e.get('rotate') else ''}</dd>"
        f"</dl><div class=actions>"
        f"<form method=post action=/refresh><input type=hidden name=mac value='{esc(mac)}'>"
        "<button class='btn small'>Refresh now</button></form>"
        f"<form method=post action=/remove onsubmit=\"return confirm('Remove {esc(mac)}?')\">"
        f"<input type=hidden name=mac value='{esc(mac)}'><button class='btn small'>Remove</button></form>"
        "</div></li>"
    )


def seen_card(mac: str, adv) -> str:
    known = adv.sdk_type in zivyobraz.MODELS
    button = ("<form method=post action=/add>"
              f"<input type=hidden name=mac value='{esc(mac)}'><button class='btn small'>Add</button></form>"
              if known else "<p>Unknown display type: can't be served yet.</p>")
    return (f"<li>{head(adv.sdk_type)}<div class=mac>{esc(mac)}</div><dl>"
            f"<dt>Colours</dt><dd>{swatches(adv.sdk_type) if known else '–'}</dd>"
            f"<dt>Battery</dt><dd>{battery(adv.battery_dv)}</dd>"
            f"<dt>Signal</dt><dd>{esc(adv.rssi)} dBm</dd>"
            f"<dt>Firmware</dt><dd>0x{adv.sw:02X} / hw 0x{adv.hw:02X}</dd></dl>{button}</li>")


def log_line(line: str) -> str:
    text = line[9:]
    if "failed" in text or "can't" in text or "error" in text:
        cls = "err"
    elif "image on the tag" in text or text.startswith("added"):
        cls = "ok"
    else:
        cls = ""
    return f"<span class=t>{esc(line[:8])}</span> <span class='{cls}'>{esc(text)}</span>\n"


def page(gw: Gateway, ring: LogRing, versions: dict[str, str]) -> str:
    css = versions["style.css"]
    out = [
        "<!doctype html><html lang=en><head><meta charset=utf-8>"
        "<meta http-equiv=refresh content=15>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta name=theme-color content='#009baa'><title>Živý obraz BLE Gateway</title>"
        f"<link rel=stylesheet href=/style.css?v={css}></head><body>"
        "<header class=top><span class=brand>Laska<b>Kit</b></span>"
        "<div class=title>Živý obraz<b>BLE Gateway</b></div></header><main>"
    ]
    tags = gw.registry.tags
    out.append(f"<section class=card><h2>Tags <small>{len(tags)}</small></h2>")
    if tags:
        out.append("<p class=hint>Add each tag on <a href='https://zivyobraz.eu/' target=_blank>zivyobraz.eu</a> "
                   "with its address (MAC) and its <b>zivyobraz key</b>.</p><ul class=displays>")
        out.extend(tag_card(gw, mac, e) for mac, e in tags.items())
        out.append("</ul>")
    else:
        out.append("<p class=empty>No tags yet. Add one from the list below.</p>")
    out.append("</section>")

    new = {mac: adv for mac, adv in sorted(gw.seen.items(), key=lambda kv: -(kv[1].rssi or -999))
           if mac not in gw.registry}
    out.append(f"<section class=card><h2>In range, not added <small>{len(new)}</small></h2>")
    if new:
        out.append("<ul class=displays>" + "".join(seen_card(mac, adv) for mac, adv in new.items()) + "</ul>")
    else:
        out.append("<p class=empty>No other Picksmart tags heard yet.</p>")
    out.append("</section>")

    out.append("<section class='card log'><h2>Log <small>newest first</small></h2><pre>")
    out.extend(log_line(line) for line in reversed(ring.lines))
    if not ring.lines:
        out.append("(nothing yet)")
    out.append("</pre></section></main><footer>Živý obraz BLE Gateway · Python</footer></body></html>")
    return "".join(out)


def file_version(data: bytes) -> str:
    """FNV-1a of a file, for its "?v=" link (a changed file gets a new URL)."""
    h = 2166136261
    for b in data:
        h = (h ^ b) * 16777619 & 0xFFFFFFFF
    return f"{h:08x}"


async def app(gw: Gateway, ring: LogRing, allow_hosts=()):
    """The aiohttp application (routes behind the guard)."""
    from aiohttp import web

    names = own_names() | {h.lower() for h in allow_hosts}

    @web.middleware
    async def guard(request, handler):
        host = request.headers.get("Host")
        why = None
        if not host_ok(host, names):
            why = "unknown host name (see --allow-host)"
        elif request.method == "POST" and not origin_ok(
                request.headers.get("Origin") or request.headers.get("Referer"), host):
            why = "from another site"
        if why:
            log.info(f"web: refused {request.method} {request.path[:40]} ({why}; host {str(host)[:40]}, "
                     f"origin {request.headers.get('Origin', '-')[:40]})")
            return web.Response(status=403, text="Refused: open the gateway's page by its own address "
                                                 "and use the buttons there.")
        return await handler(request)

    versions: dict[str, str] = {}

    def static(name: str, content_type: str):
        data = (WWW / name).read_bytes()
        versions[name] = file_version(data)

        async def handler(request):
            return web.Response(body=data, content_type=content_type, headers={"Cache-Control": "max-age=86400"})
        return handler

    async def index(request):
        return web.Response(text=page(gw, ring, versions), content_type="text/html", charset="utf-8")

    async def preview(request):
        name = request.match_info["name"]
        path = gw.preview_dir / name
        if not re.fullmatch(r"[0-9A-F]{12}\.png", name) or not path.exists():
            raise web.HTTPNotFound()
        return web.FileResponse(path)

    async def favicon(request):
        return web.Response(status=204)  # no icon (and not the page, which `other` would serve)

    async def other(request):
        # Serve the page itself, don't redirect: a browser may hold a cached permanent redirect
        # "/" -> "/dashboard/" from whatever ran on this port before, and redirecting back would loop.
        log.debug(f"web: page requested as {request.path_qs}")
        return await index(request)

    def action(do):
        async def handler(request):
            mac = (await request.post()).get("mac", "").upper()
            try:
                do(mac)
            except ValueError as err:
                log.info(f"can't: {err}")
            raise web.HTTPSeeOther("/")
        return handler

    application = web.Application(middlewares=[guard])
    application.add_routes([
        web.get("/", index),
        web.get("/style.css", static("style.css", "text/css")),
        web.get("/preview/{name}", preview),
        web.post("/add", action(gw.add)),
        web.post("/refresh", action(gw.refresh)),
        web.post("/remove", action(gw.remove)),
        web.get("/favicon.ico", favicon),
        web.get("/{tail:.*}", other),  # anything else (/dashboard/, /index.html, ...): the page
    ])
    return application


async def start(gw: Gateway, ring: LogRing, port: int, allow_hosts=()) -> None:
    from aiohttp import web

    runner = web.AppRunner(await app(gw, ring, allow_hosts), access_log=None)
    await runner.setup()
    await web.TCPSite(runner, port=port).start()
    log.info(f"web page on http://{socket.gethostname()}:{port}/")
