#!/usr/bin/env python3
"""Headless, muted browser test of web/index.html with a real ROM.

Feeds the ROM through the real <input type=file>, then checks from the page:
the core runs at real-time speed, the canvas is drawn, the game reaches play
(read from the game's own work RAM, not from a screenshot), and the tab made
no off-origin or uploading requests.

    tests/browser_test.py ROM [--game sonic1|sonic2] [--shot out.png]
"""
import argparse, functools, http.server, json, os, pathlib, socket, sys, threading, time
sys.path.insert(0, os.path.dirname(__file__))
from cdp import Chrome  # noqa: E402

WEB = pathlib.Path(__file__).resolve().parent.parent / "web"
# Work RAM offsets ($FFxxxx) from each disassembly's own symbol table.
GAMES = {
    "sonic1": dict(mode=0xF600, demo=0xFFF0, camx=0xF700, title=0x04, level=0x0C),
    "sonic2": dict(mode=0xF600, demo=0xFFF0, camx=0xEE00, title=0x04, level=0x0C),
}


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom"); ap.add_argument("--game", default="sonic1"); ap.add_argument("--shot")
    a = ap.parse_args()
    g = GAMES[a.game]

    port = free_port()
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *_):
            pass
    handler = functools.partial(Quiet, directory=str(WEB))
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{port}"
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        print(f"  {'ok  ' if cond else 'FAIL'} {name}{(' - ' + str(detail)) if detail != '' else ''}")

    with Chrome(port=free_port(), headless=True) as chrome:
        tab = chrome.tab("about:blank")
        tab.call("Network.enable")
        assert tab.goto(origin + "/index.html"), "page did not load"
        for _ in range(100):
            if tab.js("!!window.mdcore"):
                break
            time.sleep(0.1)
        check("core instantiated", tab.js("!!window.mdcore"))
        tab.set_file_input("#file", [os.path.abspath(a.rom)])
        for _ in range(100):
            if tab.js("window.mdcore.loaded && window.mdcore.frames > 0"):
                break
            time.sleep(0.1)
        check("ROM loaded from the file input", tab.js("window.mdcore.loaded"))
        check("sound is off", tab.js("window.mdcore.audio === null"))

        f0 = tab.js("window.mdcore.frames"); t0 = time.time(); time.sleep(5)
        fps = (tab.js("window.mdcore.frames") - f0) / (time.time() - t0)
        check("real-time speed", 55 <= fps <= 62, f"{fps:.1f} fps")
        cw, ch = tab.js("[document.getElementById('screen').width, document.getElementById('screen').height]")
        check("canvas has size", cw > 0 and ch > 0, f"{cw}x{ch}")
        colours = tab.js("""(() => { const c = document.getElementById('screen');
            const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; const s = new Set();
            for (let i = 0; i < d.length; i += 16) s.add(d[i] << 16 | d[i+1] << 8 | d[i+2]); return s.size; })()""")
        check("canvas shows a picture", colours > 4, f"{colours} distinct colours sampled")

        ram = lambda off: tab.js(f"window.mdcore.ram()[{off & 0xFFFF}]")
        word = lambda off: tab.js(f"(r => r[{off}] << 8 | r[{off + 1}])(window.mdcore.ram())")

        # wait for the title screen, press Start, then hold Right
        end = time.time() + 30
        while time.time() < end and ram(g["mode"]) != g["title"]:
            time.sleep(0.2)
        check("reached title screen", ram(g["mode"]) == g["title"], hex(ram(g["mode"])))
        # Start is ignored until the logo is up, and a missed press runs the
        # attract demo, so keep pressing on the title until a real level begins
        end = time.time() + 60
        while time.time() < end and not (ram(g["mode"]) == g["level"] and word(g["demo"]) == 0):
            if ram(g["mode"]) == g["title"]:
                tab.js("window.mdcore.setButtons(0, 0x80)"); time.sleep(0.25)
                tab.js("window.mdcore.setButtons(0, 0)")
            time.sleep(0.75)
        time.sleep(3)
        cam0 = word(g["camx"])
        tab.js("window.mdcore.setButtons(0, 0x08)"); time.sleep(4)
        cam1 = word(g["camx"])
        mode, demo = ram(g["mode"]), word(g["demo"])
        check("in a level, not the attract demo", mode == g["level"] and demo == 0, f"mode {mode:#x} demo {demo}")
        check("player input moves the camera", cam1 > cam0 + 64, f"camera x {cam0} -> {cam1}")
        tab.js("window.mdcore.setButtons(0, 0)")

        st = tab.js("(() => { const s = window.mdcore.saveState(); window._st = s; return s.length; })()")
        f1 = tab.js("window.mdcore.frames"); time.sleep(1)
        restored = tab.js("window.mdcore.loadState(window._st)")
        check("save state round trip", st > 0 and restored, f"{st} bytes")

        if a.shot:
            tab.screenshot(pathlib.Path(a.shot))

        reqs = [e["params"]["request"] for e in tab.events if e["method"] == "Network.requestWillBeSent"]
        off = [r["url"] for r in reqs if not r["url"].startswith(origin) and not r["url"].startswith("data:")]
        uploads = [r["url"] for r in reqs if r.get("method") not in ("GET", None)]
        check("no off-origin requests", not off, off or len(reqs))
        check("no uploading requests", not uploads, uploads or "")
        errs = [m for m in tab.console() if "exception" in m.lower() or "error" in m.lower()]
        check("no console errors", not errs, errs or "")
        chrome.close_tab(tab)
    httpd.shutdown()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
