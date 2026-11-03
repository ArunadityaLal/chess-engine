#!/usr/bin/env python3
"""Play on Lichess as a bot, with this engine. Standard library only.

Setup (once):
  1. Create a NEW Lichess account for the bot (a bot account can't play as a human, and
     an account that has played games can't become a bot).
  2. Create an API token for it with the "bot:play" scope:
     https://lichess.org/account/oauth/token/create?scopes[]=bot:play
  3. Upgrade it to a bot account. This is irreversible, so it is not done by this script:
     curl -d '' https://lichess.org/api/bot/account/upgrade -H "Authorization: Bearer $LICHESS_TOKEN"

Run:
  export LICHESS_TOKEN=...            # never commit the token
  python3 tools/lichess_bot.py
  python3 tools/lichess_bot.py --option Eval=NNUE --option NNUEFile=evals/nnue/net3.nnue \\
      --option Threads=2 --min-base 60 --max-base 900 --games 2

It accepts standard-chess challenges within the time-control limits (no correspondence or
unlimited games) and plays each game with its own engine process. Ctrl-C stops accepting
challenges and lets running games finish; a second Ctrl-C quits.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API = "https://lichess.org"


class Lichess:
    def __init__(self, token):
        self.headers = {"Authorization": f"Bearer {token}", "User-Agent": "chess-engine-bot"}

    def _request(self, method, path, data=None):
        body = urllib.parse.urlencode(data).encode() if data is not None else (b"" if method == "POST" else None)
        req = urllib.request.Request(API + path, data=body, method=method, headers=self.headers)
        for attempt in range(5):
            try:
                return urllib.request.urlopen(req, timeout=30)
            except urllib.error.HTTPError as e:
                if e.code == 429:  # rate limited: Lichess asks for a full minute's pause
                    log("rate limited; waiting 60 s")
                    time.sleep(60)
                elif e.code >= 500:
                    time.sleep(2 ** attempt)
                else:
                    raise
            except urllib.error.URLError:
                time.sleep(2 ** attempt)
        raise RuntimeError(f"{method} {path} failed repeatedly")

    def get_json(self, path):
        with self._request("GET", path) as r:
            return json.load(r)

    def post(self, path, data=None):
        try:
            with self._request("POST", path, data) as r:
                return r.status == 200
        except urllib.error.HTTPError as e:
            log(f"POST {path}: HTTP {e.code} {e.read().decode(errors='replace')[:200]}")
            return False

    def stream(self, path):
        """Yields the JSON objects of an ndjson stream (keep-alive blank lines skipped)."""
        req = urllib.request.Request(API + path, headers=self.headers)
        with urllib.request.urlopen(req, timeout=60) as r:
            for line in r:
                if line.strip():
                    yield json.loads(line)


class Engine:
    """A UCI engine process for one game."""

    def __init__(self, path, options):
        self.p = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.send("uci")
        self.wait_for("uciok")
        for name, value in options:
            self.send(f"setoption name {name} value {value}")
        self.send("ucinewgame")
        self.send("isready")
        self.wait_for("readyok")

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def wait_for(self, prefix):
        info = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("engine exited")
            if line.startswith("info") and " score " in line and " pv " in line:
                info = line
            if line.startswith(prefix):
                return line.strip(), info

    def best_move(self, start, moves, wtime, btime, winc, binc):
        pos = "position startpos" if start == "startpos" else f"position fen {start}"
        self.send(pos + (" moves " + " ".join(moves) if moves else ""))
        self.send(f"go wtime {wtime} btime {btime} winc {winc} binc {binc}")
        line, info = self.wait_for("bestmove")
        return line.split()[1], info

    def quit(self):
        try:
            self.send("quit")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def ms(value):
    """Clock values arrive in milliseconds (ints), or as dates in some events."""
    return int(value) if isinstance(value, (int, float)) else 60_000


def play_game(li, game_id, me, args):
    engine = Engine(args.engine, args.option)
    try:
        greeted = False
        for event in li.stream(f"/api/bot/game/stream/{game_id}"):
            kind = event.get("type")
            if kind == "gameFull":
                white = event["white"].get("id", "")
                color = "white" if white == me else "black"
                start = event.get("initialFen", "startpos")
                start_side = "white" if start == "startpos" or start.split()[1] == "w" else "black"
                opponent = event["black" if color == "white" else "white"]
                log(f"{game_id}: playing {color} vs {opponent.get('name', opponent.get('id', '?'))}")
                if args.greeting and not greeted:
                    li.post(f"/api/bot/game/{game_id}/chat", {"room": "player", "text": args.greeting})
                    greeted = True
                state = event["state"]
            elif kind == "gameState":
                state = event
            else:
                continue

            if state.get("status", "started") != "started":
                log(f"{game_id}: over ({state.get('status')}, winner {state.get('winner', 'none')})")
                break
            moves = state["moves"].split() if state.get("moves") else []
            to_move = start_side if len(moves) % 2 == 0 else ("black" if start_side == "white" else "white")
            if to_move != color:
                continue
            move, info = engine.best_move(start, moves, ms(state["wtime"]), ms(state["btime"]),
                                          ms(state["winc"]), ms(state["binc"]))
            if not li.post(f"/api/bot/game/{game_id}/move/{move}"):
                log(f"{game_id}: move {move} rejected")
    except Exception as e:
        log(f"{game_id}: error {e!r}")
    finally:
        engine.quit()


def acceptable(challenge, args):
    if challenge.get("variant", {}).get("key") != "standard":
        return "variant"
    tc = challenge.get("timeControl", {})
    if tc.get("type") != "clock":
        return "timeControl"
    if not args.min_base <= tc.get("limit", 0) <= args.max_base:
        return "timeControl"
    if challenge.get("rated") and not args.rated:
        return "casual"
    if not challenge.get("rated") and not args.casual:
        return "rated"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default=str(ROOT / "build" / "release" / "engine"))
    ap.add_argument("--option", action="append", default=[], metavar="NAME=VALUE",
                    help="UCI option for the engine (repeatable)")
    ap.add_argument("--games", type=int, default=1, help="games at once")
    ap.add_argument("--min-base", type=int, default=30, help="shortest base time accepted, seconds")
    ap.add_argument("--max-base", type=int, default=1800, help="longest base time accepted, seconds")
    ap.add_argument("--no-rated", dest="rated", action="store_false")
    ap.add_argument("--no-casual", dest="casual", action="store_false")
    ap.add_argument("--greeting", default="Hi! I'm a hobby engine that learns from its own games. Good luck!")
    args = ap.parse_args()
    args.option = [tuple(o.split("=", 1)) for o in args.option]

    token = os.environ.get("LICHESS_TOKEN")
    if not token:
        sys.exit("set LICHESS_TOKEN to the bot account's API token (scope bot:play)")
    li = Lichess(token)
    account = li.get_json("/api/account")
    me = account["id"]
    if account.get("title") != "BOT":
        sys.exit(f"{account['username']} is not a bot account yet; see the setup notes in this script")
    log(f"logged in as {account['username']}; waiting for challenges (Ctrl-C to stop)")

    games = {}
    accepting = True
    while True:
        try:
            for event in li.stream("/api/stream/event"):
                kind = event.get("type")
                if kind == "challenge":
                    ch = event["challenge"]
                    if ch.get("challenger", {}).get("id") == me:
                        continue  # our own outgoing challenge
                    games = {g: t for g, t in games.items() if t.is_alive()}
                    reason = acceptable(ch, args)
                    if not accepting:
                        reason = "later"
                    elif reason is None and len(games) >= args.games:
                        reason = "later"
                    if reason:
                        li.post(f"/api/challenge/{ch['id']}/decline", {"reason": reason})
                        log(f"declined {ch['id']} from {ch['challenger']['id']} ({reason})")
                    else:
                        li.post(f"/api/challenge/{ch['id']}/accept")
                        log(f"accepted {ch['id']} from {ch['challenger']['id']}")
                elif kind == "gameStart":
                    gid = event["game"]["id"]
                    if gid not in games or not games[gid].is_alive():
                        games[gid] = threading.Thread(target=play_game, args=(li, gid, me, args), daemon=True)
                        games[gid].start()
                elif kind == "gameFinish":
                    log(f"{event['game']['id']}: finished")
        except KeyboardInterrupt:
            if not accepting:
                sys.exit(0)
            accepting = False
            log("no longer accepting challenges; Ctrl-C again to quit")
        except Exception as e:
            log(f"event stream dropped ({e!r}); reconnecting")
            time.sleep(5)


if __name__ == "__main__":
    main()
