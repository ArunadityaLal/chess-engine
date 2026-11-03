#!/usr/bin/env python3
"""Play the engine in the browser, with chess.com-style premoves. Standard library only.

  python3 tools/play/server.py [--port 8765] [--engine build/release/engine]

Opens http://localhost:8765. Any number of premoves can be queued while the engine thinks;
each one is played the moment the engine has moved, without using any of your clock.
Right-click cancels them. Every game is saved to games/ after each move, and "Analyse"
runs the coach on it (tools/coach/coach.py analyze).
"""
import argparse
import errno
import json
import re
import subprocess
import sys
import threading
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "coach"))
from coach import Engine  # noqa: E402

GAMES = ROOT / "games"
STATIC = {"/": ("index.html", "text/html"), "/chess.min.js": ("chess.min.js", "text/javascript")}


def eval_choices():
    """Evaluations the engine can play with: PeSTO plus every trained weight file."""
    out = ["PeSTO"]
    out += [f"Learned {p.stem}" for p in sorted((ROOT / "evals" / "learned").glob("gen*.txt"))]
    out += [f"NNUE {p.stem}" for p in sorted((ROOT / "evals" / "nnue").glob("net*.nnue"))]
    return out


# Options the page sets through its own controls (or the server needs), not the generic list.
OWN_CONTROLS = {"Style", "Variety", "Contempt", "Eval", "EvalFile", "NNUEFile", "ThinkAloud"}
# Defaults for play that differ from the engine's own.
PLAY_DEFAULTS = {"Hash": "64"}


def engine_options(path):
    """The engine's UCI options, from its 'uci' reply, for the page's settings panel."""
    out = subprocess.run([path], input="uci\nquit\n", capture_output=True, text=True).stdout
    options = []
    for line in out.splitlines():
        m = re.match(r"option name (.+?) type (\w+)(.*)", line)
        if not m or m.group(1) in OWN_CONTROLS or m.group(2) == "button":
            continue
        name, kind, rest = m.groups()
        opt = {"name": name, "type": kind}
        d = re.search(r" default (.*?)(?= min | max | var |$)", rest)
        opt["default"] = PLAY_DEFAULTS.get(name, d.group(1).strip() if d else "")
        for key in ("min", "max"):
            v = re.search(rf" {key} (-?\d+)", rest)
            if v:
                opt[key] = int(v.group(1))
        if kind == "combo":
            opt["vars"] = re.findall(r" var (\S+)", rest)
        options.append(opt)
    return options


def eval_options(choice):
    kind, _, name = choice.partition(" ")
    if kind == "Learned":
        return [("Eval", "Learned"), ("EvalFile", ROOT / "evals" / "learned" / f"{name}.txt")]
    if kind == "NNUE":
        return [("Eval", "NNUE"), ("NNUEFile", ROOT / "evals" / "nnue" / f"{name}.nnue")]
    return [("Eval", "PeSTO")]


class Game:
    """One engine process, restarted for every new game so options start clean."""

    def __init__(self, path):
        self.path = path
        self.options = engine_options(path)
        self.engine = None
        self.lock = threading.Lock()

    def new(self, settings):
        with self.lock:
            if self.engine:
                self.engine.send("quit")
            known = {o["name"] for o in self.options}
            options = [(k, v) for k, v in PLAY_DEFAULTS.items()]
            options += [(k, v) for k, v in settings.get("options", {}).items() if k in known]
            options += [("ThinkAloud", "true"),
                       ("Style", settings.get("style", "Objective")),
                       ("Variety", int(settings.get("variety", 0))),
                       ("Contempt", int(settings.get("contempt", 0)))]
            options += eval_options(settings.get("eval", "PeSTO"))
            self.engine = Engine(self.path, options)
            self.engine.send("ucinewgame")

    def go(self, req):
        """The engine's move for the game so far, with its score and its reason."""
        with self.lock:
            e = self.engine
            e.set_position("startpos", req["moves"])
            e.send("go wtime {wtime} btime {btime} winc {winc} binc {binc}".format(
                **{k: max(0, int(req[k])) for k in ("wtime", "btime", "winc", "binc")}))
            out = e.read_until("bestmove")
        score, reason = None, ""
        for line in out:
            m = re.search(r" score (cp|mate) (-?\d+)", line)
            if m and line.startswith("info depth"):
                score = {"kind": m.group(1), "value": int(m.group(2))}
            if "[think] decision: " in line:
                reason = line.split("[think] decision: ", 1)[1]
        return {"move": out[-1].split()[1], "score": score, "reason": reason}


def safe_name(name):
    # No dots in the stem: the coach derives its report names with Path.with_suffix.
    return re.sub(r"[^A-Za-z0-9_-]", "_", name.removesuffix(".pgn")) + ".pgn"


def make_handler(game):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path in STATIC:
                file, ctype = STATIC[self.path]
                self.reply(200, (HERE / file).read_bytes(), ctype)
            elif self.path == "/api/info":
                self.reply(200, {"evals": eval_choices(), "options": game.options})
            else:
                self.reply(404, {"error": "not found"})

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            try:
                if self.path == "/api/new":
                    game.new(req)
                    self.reply(200, {"ok": True})
                elif self.path == "/api/go":
                    self.reply(200, game.go(req))
                elif self.path == "/api/save":
                    GAMES.mkdir(exist_ok=True)
                    path = GAMES / safe_name(req["file"])
                    path.write_text(req["pgn"] + "\n")
                    self.reply(200, {"path": str(path.relative_to(ROOT))})
                elif self.path == "/api/analyze":
                    path = GAMES / safe_name(req["file"])
                    out = path.with_name(path.stem + "-analysis")
                    subprocess.run([sys.executable, str(ROOT / "tools" / "coach" / "coach.py"), "analyze",
                                    str(path), "--out", str(out)], check=True, capture_output=True)
                    subprocess.run(["open", str(out.with_suffix(".html"))])
                    self.reply(200, {"report": str(out.with_suffix(".html").relative_to(ROOT))})
                else:
                    self.reply(404, {"error": "not found"})
            except Exception as ex:  # report to the page instead of dropping the connection
                self.reply(500, {"error": str(ex)})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--engine", default=str(ROOT / "build" / "release" / "engine"))
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    url = f"http://localhost:{args.port}"
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(Game(args.engine)))
    except OSError as ex:
        if ex.errno != errno.EADDRINUSE:
            raise
        try:  # our own server already running there: just show it
            urllib.request.urlopen(url + "/api/info", timeout=2)
        except OSError:
            sys.exit(f"Port {args.port} is used by another program; try --port {args.port + 1}")
        print(f"Already running at {url}")
        if not args.no_browser:
            webbrowser.open(url)
        return
    print(f"Playing at {url} (Ctrl-C to stop)")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
