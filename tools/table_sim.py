#!/usr/bin/env python3
"""Dune Weaver table simulator.

Mocks the sand-table firmware HTTP API from docs/PORTING_NOTES.md so the
ESP32 touch panel can discover, connect to, and drive a fake table on the
LAN without touching real hardware. Advertises itself over mDNS via macOS
`dns-sd` with TXT model=dune-weaver.

Stdlib only (Python 3.10+). Usage:

    python tools/table_sim.py                       # DWSIM on :8080, throttled /sd
    python tools/table_sim.py --fast                # no /sd throttle
    python tools/table_sim.py --password hunter2    # exercise the 401 path
    python tools/table_sim.py --heap-low            # heap_largest < 20000 (30 s poll backoff)
    python tools/table_sim.py --port 9090 --name DWSIM2   # second instance

Patterns are served read-only from the dune-weaver-pi library; playlists and
uploads live in tools/sim_data/ (gitignored, seeded on first run).
"""

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_PATTERNS = "/Volumes/SSD/projects/dune-weaver-pi/patterns"
SIM_DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sim_data")

CLEAR_FILES = {  # firmware clear mode -> pattern file (rel to /patterns)
    "in": "clear_from_in.thr",
    "out": "clear_from_out.thr",
    "adaptive": "clear_sideway.thr",
}

LED_DEFAULTS = {
    "LED/Effect": "rainbow", "LED/Palette": "rainbow", "LED/Brightness": "200",
    "LED/Speed": "128", "LED/Color": "ffaa55", "LED/Color2": "0000ff",
    "LED/BallBright": "255", "LED/BallBgBright": "64", "LED/BallSize": "6",
    "LED/Align": "0", "LED/BallBg": "off", "LED/Direction": "cw",
}


class Table:
    """The simulated machine: state, current job, playlist engine."""

    def __init__(self, name, patterns_root, data_root):
        self.lock = threading.RLock()
        self.name = name
        self.patterns_root = patterns_root
        self.playlists_root = os.path.join(data_root, "playlists")
        self.state = "Idle"          # Idle | Run | Hold:0 | Home | Alarm
        self.file = ""               # /sd/patterns/... while a job exists
        self.progress = -1.0
        self.feed = 200
        self.job_ends = 0.0          # monotonic deadline for the running job
        self.job_started = 0.0
        self.hold_left = 0.0         # remaining seconds captured on pause
        self.chain_next = None       # pattern to start after a one-shot clear
        self.reboot_until = 0.0
        self.led = {"effect": "rainbow", "brightness": 200}
        self.settings = {
            "THR/Feed": "200", "Playlist/Mode": "loop", "Playlist/Shuffle": "ON",
            "Playlist/PauseTime": "10800", "Playlist/ClearPattern": "adaptive",
            "Playlist/Autostart": "", **LED_DEFAULTS,
        }
        self.time_epoch_off = None   # epoch - monotonic, once pushed/synced
        self.tz = "UTC0"
        # playlist run state
        self.pl = None  # dict: name, files, index, clearing, pause_until, pause_total, phase

    # -- pattern/playlist library ------------------------------------------

    def pattern_list(self):
        out = []
        for root, _dirs, files in os.walk(self.patterns_root):
            for f in files:
                if f.endswith(".thr"):
                    rel = os.path.relpath(os.path.join(root, f), self.patterns_root)
                    out.append(rel.replace(os.sep, "/"))
        return sorted(out)

    def playlist_files(self, name):
        path = os.path.join(self.playlists_root, name + ".txt")
        files = []
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("/sd/"):
                    line = line[3:]
                if not line.startswith("/patterns/"):
                    line = "/patterns/" + line.lstrip("/")
                files.append(line)
        return files

    # -- job control --------------------------------------------------------

    def _duration_for(self, sd_rel):
        """Seconds to 'weave' a file: scaled from size, kept demo-short."""
        try:
            size = os.path.getsize(os.path.join(self.patterns_root,
                                                sd_rel[len("/patterns/"):]))
        except OSError:
            size = 60000
        return min(180.0, max(25.0, size / 2500.0))

    def start_file(self, sd_rel):
        """sd_rel like /patterns/x.thr"""
        now = time.monotonic()
        self.state = "Run"
        self.file = "/sd" + sd_rel
        self.job_started = now
        self.job_ends = now + self._duration_for(sd_rel)
        self.progress = 0.0

    def stop(self):
        self.state = "Idle"
        self.file = ""
        self.progress = -1.0
        self.pl = None
        self.chain_next = None

    def pause(self):
        if self.state == "Run":
            self.hold_left = max(0.0, self.job_ends - time.monotonic())
            self.state = "Hold:0"

    def resume(self):
        if self.state == "Hold:0":
            self.job_ends = time.monotonic() + self.hold_left
            self.state = "Run"

    def home(self):
        self.stop()
        self.state = "Home"
        self.file = ""
        self.job_ends = time.monotonic() + 8.0
        self.progress = -1.0

    def run_playlist(self, name):
        files = self.playlist_files(name)
        if not files:
            return
        if self.settings.get("Playlist/Shuffle", "OFF") == "ON":
            files = random.sample(files, len(files))
        self.pl = {
            "name": name, "files": files, "index": 0,
            "phase": "clear",  # clear -> run -> pause -> (next) clear ...
            "pause_until": 0.0,
            "pause_total": int(self.settings.get("Playlist/PauseTime", "0") or 0),
        }
        self._pl_start_current()

    def _clear_file(self):
        mode = self.settings.get("Playlist/ClearPattern", "adaptive")
        rel = CLEAR_FILES.get(mode)
        if mode in ("none", "") or rel is None:
            return None
        if not os.path.exists(os.path.join(self.patterns_root, rel)):
            return None
        return "/patterns/" + rel

    def _pl_start_current(self):
        clear = self._clear_file()
        if self.pl["phase"] == "clear" and clear:
            self.start_file(clear)
            self.job_ends = self.job_started + 15.0  # clears run short in sim
        else:
            self.pl["phase"] = "run"
            self.start_file(self.pl["files"][self.pl["index"]])

    def skip(self):
        if self.pl:
            self._pl_advance()

    def _pl_advance(self):
        pl = self.pl
        pl["index"] += 1
        if pl["index"] >= len(pl["files"]):
            if self.settings.get("Playlist/Mode") == "loop":
                pl["index"] = 0
                if self.settings.get("Playlist/Shuffle", "OFF") == "ON":
                    pl["files"] = random.sample(pl["files"], len(pl["files"]))
            else:
                self.stop()
                return
        pl["phase"] = "clear"
        self._pl_start_current()

    # -- tick + status ------------------------------------------------------

    def tick(self):
        now = time.monotonic()
        if self.state == "Home" and now >= self.job_ends:
            self.state = "Idle"
        if self.state == "Run":
            total = self.job_ends - self.job_started
            self.progress = min(1.0, (now - self.job_started) / total) if total > 0 else 1.0
            if now >= self.job_ends:
                if self.chain_next:
                    target, self.chain_next = self.chain_next, None
                    self.start_file(target)
                elif not self.pl:
                    self.stop()
                elif self.pl["phase"] == "clear":
                    self.pl["phase"] = "run"
                    self._pl_start_current()
                else:
                    # pattern finished -> rest, then advance
                    pause = self.pl["pause_total"]
                    if pause > 0:
                        self.pl["phase"] = "pause"
                        self.pl["pause_until"] = now + pause
                        self.state = "Idle"
                        self.progress = -1.0
                    else:
                        self._pl_advance()
        if self.pl and self.pl["phase"] == "pause" and now >= self.pl["pause_until"]:
            self._pl_advance()

    def status(self, heap_low):
        self.tick()
        now = time.monotonic()
        pl = self.pl
        playlist = {
            "active": False, "index": 0, "total": 0, "name": "",
            "clearing": False, "pause_remaining": -1, "pause_total": -1,
            "next": "", "last": "",
        }
        if pl:
            idx = pl["index"]
            playlist.update({
                "active": True, "index": idx, "total": len(pl["files"]),
                "name": pl["name"], "clearing": pl["phase"] == "clear",
                "pause_total": pl["pause_total"] if pl["phase"] == "pause" else -1,
                "pause_remaining": max(0, int(pl["pause_until"] - now)) if pl["phase"] == "pause" else -1,
                "next": pl["files"][(idx + 1) % len(pl["files"])],
                "last": pl["files"][idx],
            })
        running = self.state in ("Run", "Hold:0") or bool(pl)
        return {
            "state": self.state,
            "hostname": self.name.lower(),
            "file": self.file,
            "running": running,
            "feed": self.feed,
            "progress": self.progress if self.state in ("Run", "Hold:0") else (0.0 if running else -1.0),
            "heap_largest": 14000 if heap_low else 46000,
            "led": dict(self.led),
            "playlist": playlist,
        }

    # -- $-commands ---------------------------------------------------------

    def command(self, plain):
        """Handle GET /command?plain=<$-command>."""
        if not plain.startswith("$"):
            return
        body = plain[1:]
        key, _, val = body.partition("=")
        if key == "Bye":
            self.reboot_until = time.monotonic() + 4.0
            self.stop()
        elif key == "Playlist/Run":
            self.run_playlist(val)
        elif key == "Playlist/Skip" or plain == "$Playlist/Skip":
            self.skip()
        elif key in ("SD/Run", "Sand/Run"):
            # $Sand/Run=/patterns/x.thr clear=<mode>
            parts = val.split()
            target = parts[0]
            clear = "none"
            for p in parts[1:]:
                if p.startswith("clear="):
                    clear = p[len("clear="):]
            if target.startswith("/sd/"):
                target = target[3:]
            self.pl = None
            if key == "Sand/Run" and clear != "none":
                rel = CLEAR_FILES.get(clear, CLEAR_FILES["adaptive"])
                self.start_file("/patterns/" + rel)
                self.job_ends = self.job_started + 12.0  # clears run short in sim
                self.chain_next = target  # tick() starts this when the clear ends
            else:
                self.start_file(target)
        elif "/" in key:  # generic NVS setting write (idle-gated on real fw)
            self.settings[key] = val
            if key == "THR/Feed":
                try:
                    self.feed = int(float(val))
                except ValueError:
                    pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "DuneWeaverSim/1.0"

    # injected by main(): table, password, heap_low, sd_bps
    def log_message(self, fmt, *args):
        sys.stderr.write("[sim] %s %s\n" % (self.address_string(), fmt % args))

    def _json(self, obj, code=200, headers=None):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _text(self, body="ok", code=200):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _gate(self):
        """Reboot simulation + password check. Returns False if handled."""
        t = self.server.table
        if time.monotonic() < t.reboot_until:
            # board is "rebooting": drop the connection like a dead socket
            self.close_connection = True
            try:
                self.connection.close()
            except OSError:
                pass
            return False
        if self.server.password:
            if self.headers.get("X-Sand-Key") != self.server.password:
                self._json({"error": "unauthorized"}, code=401)
                return False
        return True

    def do_GET(self):
        if not self._gate():
            return
        url = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(url.query, keep_blank_values=True))
        t = self.server.table
        route = url.path

        with t.lock:
            if route == "/":
                s = t.status(self.server.heap_low)
                pl = s["playlist"]
                rows = {
                    "state": s["state"], "file": s["file"] or "—",
                    "progress": ("%d%%" % round(s["progress"] * 100)) if s["progress"] >= 0 else "—",
                    "feed": "%s mm/min" % s["feed"],
                    "playlist": ("%s (%d/%d)%s" % (pl["name"], pl["index"] + 1, pl["total"],
                                                   " · clearing" if pl["clearing"] else ""))
                                if pl["active"] else "—",
                    "led": "%s @ %s" % (s["led"]["effect"], s["led"]["brightness"]),
                    "patterns": str(len(t.pattern_list())),
                    "password": "required" if self.server.password else "off",
                    "/sd throttle": ("%d KB/s" % (self.server.sd_bps // 1000)) if self.server.sd_bps else "off",
                }
                trs = "".join("<tr><td>%s</td><td>%s</td></tr>" % (k, v) for k, v in rows.items())
                html = ("<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=2>"
                        "<title>%s — table sim</title>"
                        "<style>body{font:14px ui-monospace,monospace;background:#1b1712;color:#d8b578;"
                        "padding:2em}td{padding:.2em 1em .2em 0}td:first-child{color:#8a7a5c}</style>"
                        "<h2>%s — Dune Weaver table simulator</h2><table>%s</table>"
                        "<p style='color:#8a7a5c'>API: /sand_status /sand_patterns /sand_playlists "
                        "/sand_settings /sand_time /sd/&lt;path&gt; /command?plain=$… — see "
                        "docs/PORTING_NOTES.md</p>") % (t.name, t.name, trs)
                data = html.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            if route == "/sand_status":
                time.sleep(random.uniform(0.05, 0.4))  # real boards are laggy
                return self._json(t.status(self.server.heap_low))
            if route == "/sand_patterns":
                pats = t.pattern_list()
                etag = '"%s"' % hashlib.sha1("\n".join(pats).encode()).hexdigest()[:16]
                if self.headers.get("If-None-Match") == etag:
                    self.send_response(304)
                    self.send_header("ETag", etag)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                return self._json(pats, headers={"ETag": etag})
            if route == "/sand_playlists":
                names = sorted(f for f in os.listdir(t.playlists_root) if f.endswith(".txt"))
                return self._json(names)
            if route == "/sand_settings":
                return self._json(dict(t.settings))
            if route == "/sand_time":
                if "epoch" in q:
                    try:
                        t.time_epoch_off = int(q["epoch"]) - time.monotonic()
                        t.tz = q.get("tz", t.tz)
                    except ValueError:
                        pass
                synced = t.time_epoch_off is not None
                epoch = int(t.time_epoch_off + time.monotonic()) if synced else 0
                return self._json({"epoch": epoch, "synced": synced,
                                   "local": time.strftime("%H:%M:%S"), "tz": t.tz})
            if route == "/sand_stop":
                t.stop()
                return self._text()
            if route == "/sand_pause":
                t.pause()
                return self._text()
            if route == "/sand_resume":
                t.resume()
                return self._text()
            if route == "/sand_home":
                t.home()
                return self._text()
            if route == "/sand_goto":
                if t.state != "Idle":
                    return self._text("busy", code=409)
                return self._text()
            if route == "/sand_feed":
                try:
                    t.feed = int(float(q.get("mm", t.feed)))
                    t.settings["THR/Feed"] = str(t.feed)
                except ValueError:
                    pass
                return self._text()
            if route == "/sand_led":
                led_keys = {"effect": "LED/Effect", "palette": "LED/Palette",
                            "color": "LED/Color", "color2": "LED/Color2",
                            "brightness": "LED/Brightness", "speed": "LED/Speed",
                            "fgbright": "LED/BallBright", "bgbright": "LED/BallBgBright",
                            "size": "LED/BallSize", "align": "LED/Align",
                            "direction": "LED/Direction", "bg": "LED/BallBg"}
                for k, v in q.items():
                    if k in led_keys:
                        t.settings[led_keys[k]] = v
                if "effect" in q:
                    t.led["effect"] = q["effect"]
                if "brightness" in q:
                    try:
                        t.led["brightness"] = int(q["brightness"])
                    except ValueError:
                        pass
                return self._text()
            if route == "/command":
                t.command(q.get("plain", ""))
                return self._text()
            if route == "/upload":  # ESP3D file ops arrive as GET
                return self._file_op(q)

        # /sd/<path> is served outside the lock (it's slow by design)
        if route.startswith("/sd/"):
            return self._serve_sd(urllib.parse.unquote(route[len("/sd"):]))
        self._text("not found", code=404)

    def _sd_local_path(self, rel):
        t = self.server.table
        if rel.startswith("/patterns/"):
            return os.path.join(t.patterns_root, rel[len("/patterns/"):])
        if rel.startswith("/playlists/"):
            return os.path.join(t.playlists_root, rel[len("/playlists/"):])
        return None

    def _serve_sd(self, rel):
        local = self._sd_local_path(rel)
        if not local or not os.path.isfile(local):
            return self._text("not found", code=404)
        size = os.path.getsize(local)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        bps = self.server.sd_bps
        with open(local, "rb") as fh:
            while chunk := fh.read(4096):
                self.wfile.write(chunk)
                if bps:
                    time.sleep(len(chunk) / bps)

    def _file_op(self, q):
        t = self.server.table
        action = q.get("action", "")
        rel = q.get("filename") or q.get("path") or ""
        local = self._sd_local_path(rel if rel.startswith("/") else "/" + rel)
        if action == "delete" and local and os.path.isfile(local):
            if local.startswith(t.patterns_root):
                return self._text("patterns are read-only in sim", code=403)
            os.remove(local)
            return self._text()
        if action == "createdir":
            if local:
                os.makedirs(local, exist_ok=True)
            return self._text()
        if action == "rename" and local:
            new = q.get("newname", "")
            nlocal = self._sd_local_path(new if new.startswith("/") else "/" + new)
            if nlocal:
                shutil.move(local, nlocal)
            return self._text()
        return self._text("bad file op", code=400)

    def do_POST(self):
        if not self._gate():
            return
        url = urllib.parse.urlsplit(self.path)
        if url.path != "/upload":
            return self._text("not found", code=404)
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        ctype = self.headers.get("Content-Type", "")
        m = re.search(r"boundary=([^;]+)", ctype)
        if not m:
            return self._text("no boundary", code=400)
        saved = self._save_multipart(body, m.group(1).strip('"'))
        if saved is None:
            return self._text("no file part", code=400)
        return self._text("uploaded " + saved)

    def _save_multipart(self, body, boundary):
        """Minimal multipart parse: save the part that has a filename."""
        delim = b"--" + boundary.encode()
        for part in body.split(delim):
            if b"\r\n\r\n" not in part:
                continue
            head, _, data = part.partition(b"\r\n\r\n")
            fm = re.search(rb'filename="([^"]+)"', head)
            if not fm:
                continue
            data = data.rstrip(b"\r\n-")
            rel = fm.group(1).decode()
            local = self._sd_local_path(rel if rel.startswith("/") else "/" + rel)
            if not local:
                # fall back to the ?path= dir + bare name
                q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(self.path).query))
                base = q.get("path", "/playlists")
                local = self._sd_local_path(base.rstrip("/") + "/" + os.path.basename(rel))
            if not local or local.startswith(self.server.table.patterns_root):
                return None
            os.makedirs(os.path.dirname(local), exist_ok=True)
            with open(local, "wb") as fh:
                fh.write(data)
            return rel
        return None


def seed_data(table):
    os.makedirs(table.playlists_root, exist_ok=True)
    if any(f.endswith(".txt") for f in os.listdir(table.playlists_root)):
        return
    pats = [p for p in table.pattern_list() if not p.startswith("clear")]
    random.seed(42)  # deterministic seed data
    demo = {
        "Sim Favorites": random.sample(pats, min(6, len(pats))),
        "Quick Demo": random.sample(pats, min(3, len(pats))),
    }
    for name, files in demo.items():
        with open(os.path.join(table.playlists_root, name + ".txt"), "w") as fh:
            fh.write("# seeded by table_sim.py\n")
            for f in files:
                fh.write("/patterns/%s\n" % f)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", default="DWSIM", help="mDNS instance/hostname")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--patterns", default=DEFAULT_PATTERNS)
    ap.add_argument("--data", default=SIM_DATA)
    ap.add_argument("--password", default=None, help="require X-Sand-Key (401 path)")
    ap.add_argument("--fast", action="store_true", help="disable /sd 45 KB/s throttle")
    ap.add_argument("--heap-low", action="store_true", help="report heap_largest < 20000")
    ap.add_argument("--no-mdns", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(args.patterns):
        sys.exit("patterns dir not found: %s" % args.patterns)

    table = Table(args.name, args.patterns, args.data)
    seed_data(table)

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    server.table = table
    server.password = args.password
    server.heap_low = args.heap_low
    server.sd_bps = 0 if args.fast else 45_000

    dns = None
    if not args.no_mdns:
        dns = subprocess.Popen(
            ["dns-sd", "-R", args.name, "_http._tcp", "local", str(args.port),
             "model=dune-weaver", "api=sandtable/1"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    n = len(table.pattern_list())
    print("[sim] %s: http://0.0.0.0:%d — %d patterns, playlists in %s" %
          (args.name, args.port, n, table.playlists_root))
    print("[sim] mDNS: %s (_http._tcp, model=dune-weaver)" %
          ("advertising as %s.local" % args.name if dns else "off"))
    if args.password:
        print("[sim] password required (X-Sand-Key: %s)" % args.password)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if dns:
            dns.terminate()


if __name__ == "__main__":
    main()
