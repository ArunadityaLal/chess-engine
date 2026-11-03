#!/usr/bin/env python3
"""Chess coach built on the engine: analyses games, explains mistakes, finds puzzles,
shows what openings the engine discovered, and plays against you with feedback.

Everything it says comes from the engine (searches, evaluations, threats); the text is
filled into templates, so it never claims more than the engine found.

  coach.py analyze game.pgn [--game N] [--time MS] [--out report]   -> report.html + report.pgn
  coach.py puzzles games.pgn... [--time MS] [--out puzzles]         -> puzzles.pgn + puzzles.html
  coach.py selfplay --games N [--out games.pgn] [--variety CP]      book-free self-play games
  coach.py openings games.pgn... [--plies N]                        tree of the openings played
  coach.py play [--color white|black] [--time MS]                   play with hints and feedback

Engine options for any command: --option Eval=NNUE --option NNUEFile=evals/nnue/net3.nnue
Standard library only.
"""
import argparse
import html
import math
import random
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ENGINE = ROOT / "build" / "release" / "engine"
VALUES = {"p": 1, "n": 3, "b": 3, "r": 5, "q": 9}
NAMES = {"p": "a pawn", "n": "a knight", "b": "a bishop", "r": "a rook", "q": "the queen"}
MATE = 10000


# ---------------------------------------------------------------------------- engine

class Engine:
    def __init__(self, path=ENGINE, options=()):
        self.p = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.send("uci")
        self.read_until("uciok")
        for name, value in options:
            self.send(f"setoption name {name} value {value}")
        self.multipv = 1

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def read_until(self, prefix):
        lines = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("engine exited")
            lines.append(line.rstrip("\n"))
            if line.startswith(prefix):
                return lines

    def set_position(self, start, moves):
        pos = "position startpos" if start == "startpos" else f"position fen {start}"
        self.send(pos + (" moves " + " ".join(moves) if moves else ""))

    def analyse(self, start, moves, movetime, multipv=1):
        """Best lines of a position: [(score for the side to move, [uci moves])], best first."""
        if multipv != self.multipv:
            self.send(f"setoption name MultiPV value {multipv}")
            self.multipv = multipv
        self.set_position(start, moves)
        self.send(f"go movetime {movetime}")
        lines = {}
        for line in self.read_until("bestmove"):
            m = re.search(r" multipv (\d+) score (cp|mate) (-?\d+).* pv (.+)$", line)
            if m and line.startswith("info depth"):
                n, kind, value = int(m.group(1)), m.group(2), int(m.group(3))
                score = value if kind == "cp" else (MATE - abs(value)) * (1 if value > 0 else -1)
                lines[n] = (score, m.group(4).split())
        if not lines:  # no legal moves: mate or stalemate
            return []
        return [lines[k] for k in sorted(lines)]

    def query(self, start, moves, command, prefix):
        self.set_position(start, moves)
        self.send(command)
        self.send("isready")
        out = self.read_until("readyok")
        return [l for l in out if l.startswith(prefix)]

    def san(self, start, moves, line):
        out = self.query(start, moves, "sanline " + " ".join(line), "info string san")
        return out[0][len("info string san "):].split() if out else []

    def fen(self, start, moves):
        """FEN after the moves (an illegal move stops the list there)."""
        self.set_position(start, moves)
        self.send("fen")
        while True:
            line = self.p.stdout.readline().strip()
            if not line.startswith("info"):
                return line

    def motifs(self, start, moves, move):
        """Tactical patterns of a move, e.g. ["fork: Nf7 attacks Qd8 and Rh8", "check"]."""
        out = self.query(start, moves, f"motifs {move}", "info string motifs")
        text = out[0][len("info string motifs "):] if out else ""
        return [] if text in ("", "illegal move") else [m.strip() for m in text.split(";")]

    def threats(self, start, moves):
        out = self.query(start, moves, "threats 6", "info string threats")
        return out[0].replace("info string threats: ", "") if out else ""

    def quit(self):
        try:
            self.send("quit")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


# ---------------------------------------------------------------------------- chess facts

def win_prob(cp):
    """Chance to win (draws half) for the side with this score; Lichess's fitted curve."""
    return 1 / (1 + math.exp(-0.00368208 * max(-MATE, min(MATE, cp))))


def material(fen):
    """Material per side (P 1, N 3, B 3, R 5, Q 9) from a FEN."""
    board = fen.split()[0]
    white = sum(VALUES.get(c.lower(), 0) for c in board if c.isupper())
    black = sum(VALUES.get(c, 0) for c in board if c.islower())
    return white, black


def pieces(fen):
    board = fen.split()[0]
    return Counter(c for c in board if c.isalpha())


def fmt(score):
    if abs(score) >= MATE - 500:
        n = MATE - abs(score)
        return f"#{n if score > 0 else -n}"
    return f"{score / 100:+.2f}"


def side_of(start, ply):
    first = "w" if start == "startpos" else start.split()[1]
    return ("w" if first == "w" else "b") if ply % 2 == 0 else ("b" if first == "w" else "w")


def numbered(sans, start, ply):
    """SAN list with move numbers, starting at a ply of the game."""
    first_white = start == "startpos" or start.split()[1] == "w"
    full = (int(start.split()[5]) if start != "startpos" else 1)
    out = []
    for i, san in enumerate(sans):
        p = ply + i + (0 if first_white else 1)
        num = full + p // 2
        if p % 2 == 0:
            out.append(f"{num}. {san}")
        elif i == 0:
            out.append(f"{num}... {san}")
        else:
            out.append(san)
    return " ".join(out)


# ---------------------------------------------------------------------------- PGN

def read_pgn(path):
    """Games in a PGN file: [(headers, [san moves])]."""
    text = Path(path).read_text(errors="replace")
    games = []
    for chunk in re.split(r"\n\s*\n(?=\[)", text.strip()):
        headers = dict(re.findall(r'\[(\w+)\s+"([^"]*)"\]', chunk))
        body = re.sub(r"\[[^\]]*\]", " ", chunk)
        body = re.sub(r"\{[^}]*\}", " ", body)
        body = re.sub(r";[^\n]*", " ", body)
        while re.search(r"\([^()]*\)", body):
            body = re.sub(r"\([^()]*\)", " ", body)
        body = re.sub(r"\$\d+", " ", body)
        moves = [t for t in body.split()
                 if not re.fullmatch(r"\d+\.+|\.+|1-0|0-1|1/2-1/2|\*", t)]
        moves = [re.sub(r"^\d+\.+", "", t) for t in moves]
        if headers or moves:
            games.append((headers, [m for m in moves if m]))
    return games


def start_of(headers):
    return headers.get("FEN", "startpos") if headers.get("SetUp") == "1" or "FEN" in headers else "startpos"


# ---------------------------------------------------------------------------- analysis

NAG = {"brilliant": "$3", "only move": "$1", "inaccuracy": "$6", "mistake": "$2", "blunder": "$4"}
SYMBOL = {"brilliant": "!!", "only move": "!", "inaccuracy": "?!", "mistake": "?", "blunder": "??"}
COLOR = {"brilliant": "#1baca6", "only move": "#5c8bb0", "best": "#7fa650", "excellent": "#96bc4b",
         "good": "#a5b98b", "inaccuracy": "#e6b422", "mistake": "#e58f2a", "blunder": "#ca3431"}


def classify(loss, played_best, gap, dip, before):
    if played_best and dip <= -2 and 0.3 <= before < 0.97:
        return "brilliant"
    if played_best and gap >= 0.2:
        return "only move"
    if played_best:
        return "best"
    for limit, name in ((0.02, "excellent"), (0.05, "good"), (0.10, "inaccuracy"), (0.20, "mistake")):
        if loss < limit:
            return name
    return "blunder"


def material_track(eng, start, moves, line, mover, plies=7):
    """The mover's material balance after each ply of a line, relative to the start,
    with the FEN there: [(change, fen)], starting with (0, start FEN)."""
    def balance(fen):
        w, b = material(fen)
        return w - b if mover == "w" else b - w
    fen0 = eng.fen(start, moves)
    track, base = [(0, fen0)], balance(fen0)
    for i in range(1, min(len(line), plies) + 1):
        fen = eng.fen(start, moves + line[:i])
        track.append((balance(fen) - base, fen))
    return track


def persistent_dip(track):
    """Lowest balance that lasts two plies in a row (a trade in progress dips for one
    ply only), and the FEN where it stands. Same rule as the engine's Style::material_dip."""
    dip, where = 0, track[0][1]
    for (a, fa), (b, fb) in zip(track[1:], track[2:]):
        if max(a, b) < dip:
            dip, where = max(a, b), fa if a >= b else fb
    return dip, where


def material_dip(eng, start, moves, line, mover):
    return persistent_dip(material_track(eng, start, moves, line, mover))[0]


TACTICS = ("checkmate", "fork", "double check", "discovered check", "discovered attack", "skewer", "pin")


def tactic(motifs):
    """The most telling tactical motif in a list, or None."""
    for name in TACTICS:
        for m in motifs:
            if m.split(":")[0] == name:
                return m
    return None


def explain(eng, start, moves, ply, mv, before, after, best_line, refutation, cls, gap, dip):
    """Plain-language comment for a move, from engine facts only."""
    mover = side_of(start, ply)
    best_san = eng.san(start, moves, best_line[:6])
    if cls in ("brilliant", "only move"):
        t = tactic(eng.motifs(start, moves, mv))
        t = f" ({t})" if t else ""
        if cls == "brilliant":
            return f"Brilliant{t}: gives up material ({-dip} points) and still wins. {numbered(best_san, start, ply)}"
        return f"The only good move{t}: anything else is at least {gap:.0%} worse in winning chances."
    if cls in ("best", "excellent", "good"):
        return ""

    played_after = moves + [mv]
    reply_san = eng.san(start, played_after, refutation[:6])
    why = ""
    if before >= MATE - 500 and after < MATE - 500:
        why = f"misses a mate in {MATE - before}"
    elif after <= -(MATE - 500):
        why = f"allows mate in {MATE + after}: {numbered(reply_san, start, ply + 1)}"
    elif any("=" in s for s in reply_san[0::2]):  # the opponent promotes in the line
        why = f"lets the opponent promote a pawn: {numbered(reply_san, start, ply + 1)}"
    else:
        track = material_track(eng, start, moves, [mv] + refutation, mover)
        worst, where = persistent_dip(track)
        if worst <= -1:
            lost = pieces(track[0][1]) - pieces(where)
            own = [c for c in lost if (c.isupper() if mover == "w" else c.islower())]
            biggest = max(own, key=lambda c: VALUES.get(c.lower(), 0), default="p")
            t = tactic(eng.motifs(start, played_after, refutation[0])) if refutation else None
            how = f" to a {t.split(':')[0]} ({t.split(': ', 1)[1]})" if t and ": " in t else f" to a {t}" if t else ""
            why = f"loses {NAMES[biggest.lower()]}{how}: {numbered(reply_san, start, ply + 1)}"
        else:
            why = f"the position drops from {fmt(before)} to {fmt(after)}"
            if reply_san:
                why += f" after {numbered(reply_san[:3], start, ply + 1)}"
    return f"{cls.capitalize()}: {why}. Better was {best_san[0] if best_san else '?'} ({fmt(before)}): {numbered(best_san, start, ply)}"


def analyse_game(eng, headers, sans, movetime, log=print):
    start = start_of(headers)
    moves, results = [], []
    analysis = eng.analyse(start, moves, movetime, multipv=2)
    for ply, san in enumerate(sans):
        if not analysis:
            break
        mover = side_of(start, ply)
        best_score, best_line = analysis[0]
        second = analysis[1][0] if len(analysis) > 1 else None
        played_line_after = moves + [san]
        after_analysis = eng.analyse(start, played_line_after, movetime, multipv=2)
        # No reply at all: the move mated (or stalemated).
        after = -after_analysis[0][0] if after_analysis else (MATE if san.endswith("#") else 0)
        played_best = best_line and eng.san(start, moves, best_line[:1]) == [re.sub(r"[!?]+$", "", san)]
        if played_best:
            after = best_score
        before_p, after_p = win_prob(best_score), win_prob(after)
        loss = max(0.0, before_p - after_p)
        gap = before_p - win_prob(second) if second is not None else 0
        dip = material_dip(eng, start, moves, best_line, mover) if played_best else 0
        refutation = after_analysis[0][1] if after_analysis else []
        cls = classify(loss, played_best, gap, dip, before_p)
        if best_score >= MATE - 500 and after < MATE - 500 and cls in ("excellent", "good", "inaccuracy"):
            cls = "mistake"  # a forced mate let go: winning chances barely move, but it matters
        comment = explain(eng, start, moves, ply, best_line[0] if played_best else san, best_score, after,
                          best_line, refutation, cls, gap, dip)
        white_score = after if mover == "w" else -after
        results.append({"ply": ply, "san": san, "mover": mover, "class": cls, "loss": loss,
                        "before": best_score, "after": after, "white": white_score, "comment": comment,
                        "accuracy": max(0, min(100, 103.1668 * math.exp(-0.04354 * 100 * loss) - 3.1669))})
        if cls in ("inaccuracy", "mistake", "blunder", "brilliant", "only move"):
            log(f"  {numbered([san], start, ply)} {SYMBOL.get(cls, '')} {comment}")
        moves = played_line_after
        analysis = after_analysis
    return start, results


def write_annotated_pgn(path, headers, start, results):
    out = [f'[{k} "{v}"]' for k, v in headers.items()]
    out.append('[Annotator "engine coach"]')
    body = []
    for r in results:
        num = numbered([r["san"]], start, r["ply"])
        body.append(num + (" " + NAG[r["class"]] if r["class"] in NAG else ""))
        note = f"[%eval {r['white'] / 100:.2f}]" if abs(r["white"]) < MATE - 500 else ""
        if r["comment"]:
            note += " " + r["comment"]
        if note:
            body.append("{" + note.strip() + "}")
    body.append(headers.get("Result", "*"))
    Path(path).write_text("\n".join(out) + "\n\n" + wrap(" ".join(body)) + "\n")


def wrap(text, width=80):
    lines, line = [], ""
    for word in text.split(" "):
        if len(line) + len(word) + 1 > width:
            lines.append(line)
            line = word
        else:
            line = (line + " " + word).strip()
    return "\n".join(lines + [line])


def eval_graph(results, width=760, height=220):
    """SVG of White's winning chances over the game, with the notable moves marked."""
    n = max(len(results), 2)
    x = lambda i: 30 + i * (width - 40) / (n - 1)
    y = lambda r: 10 + (1 - win_prob(r["white"])) * (height - 20)
    pts = " ".join(f"{x(i):.1f},{y(r):.1f}" for i, r in enumerate(results))
    mid = 10 + (height - 20) / 2
    marks = "".join(
        f'<circle cx="{x(i):.1f}" cy="{y(r):.1f}" r="4" fill="{COLOR[r["class"]]}"><title>'
        f'{html.escape(r["san"])}: {r["class"]}</title></circle>'
        for i, r in enumerate(results) if r["class"] in ("brilliant", "only move", "inaccuracy", "mistake", "blunder"))
    area = f"30,{height - 10} {pts} {x(len(results) - 1):.1f},{height - 10}"
    return (f'<svg viewBox="0 0 {width} {height}" class="graph" role="img" aria-label="evaluation graph">'
            f'<rect x="30" y="10" width="{width - 40}" height="{height - 20}" class="bg"/>'
            f'<polygon points="{area}" class="white-area"/>'
            f'<line x1="30" x2="{width - 10}" y1="{mid}" y2="{mid}" class="mid"/>'
            f'<polyline points="{pts}" class="line"/>{marks}'
            f'<text x="2" y="18" class="axis">W</text><text x="2" y="{height - 6}" class="axis">B</text></svg>')


def write_report(path, headers, start, results):
    title = f"{headers.get('White', '?')} vs {headers.get('Black', '?')}"
    rows = []
    for r in results:
        num = numbered([r["san"]], start, r["ply"])
        rows.append(f'<tr><td class="mv">{html.escape(num)}{SYMBOL.get(r["class"], "")}</td>'
                    f'<td><span class="tag" style="background:{COLOR[r["class"]]}">{r["class"]}</span></td>'
                    f'<td class="num">{fmt(r["white"])}</td><td>{html.escape(r["comment"])}</td></tr>')
    summary = []
    for side, name in (("w", headers.get("White", "White")), ("b", headers.get("Black", "Black"))):
        mine = [r for r in results if r["mover"] == side]
        if not mine:
            continue
        acc = sum(r["accuracy"] for r in mine) / len(mine)
        counts = Counter(r["class"] for r in mine)
        parts = ", ".join(f"{counts[c]} {c}" for c in ("brilliant", "only move", "inaccuracy", "mistake", "blunder") if counts[c])
        summary.append(f"<li><b>{html.escape(name)}</b>: accuracy {acc:.0f}%{'; ' + parts if parts else ''}</li>")
    page = f"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>{html.escape(title)}</title>
<style>
:root {{ --bg:#fafaf7; --fg:#222; --muted:#666; --card:#fff; --line:#333; --area:#e9e9e4; --border:#ddd; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#1b1b1d; --fg:#e8e8e8; --muted:#9a9a9a; --card:#242427;
  --line:#ddd; --area:#3a3a3e; --border:#3a3a3e; }} }}
body {{ background:var(--bg); color:var(--fg); font:15px/1.5 system-ui, sans-serif; margin:0 auto;
  max-width:820px; padding:16px; }}
h1 {{ font-size:1.4em; margin:0 0 4px }} .sub {{ color:var(--muted); margin:0 0 16px }}
.graph {{ width:100%; height:auto; background:var(--card); border:1px solid var(--border); border-radius:6px }}
.graph .bg {{ fill:#555 }} .graph .white-area {{ fill:#f2f2f2 }} .graph .mid {{ stroke:#999; stroke-dasharray:4 }}
.graph .line {{ fill:none; stroke:#c33; stroke-width:1.5 }} .graph .axis {{ fill:var(--muted); font-size:11px }}
table {{ border-collapse:collapse; width:100%; margin-top:16px; background:var(--card) }}
td {{ border-top:1px solid var(--border); padding:4px 8px; vertical-align:top }}
.mv {{ white-space:nowrap; font-weight:600 }} .num {{ font-variant-numeric:tabular-nums; white-space:nowrap }}
.tag {{ color:#fff; border-radius:4px; padding:1px 6px; font-size:.8em; white-space:nowrap }}
@media (max-width:600px) {{ td:nth-child(4) {{ font-size:.9em }} }}
</style></head><body>
<h1>{html.escape(title)}</h1>
<p class="sub">{html.escape(headers.get('Event', ''))} {html.escape(headers.get('Date', ''))} · result {html.escape(headers.get('Result', '*'))}</p>
{eval_graph(results)}
<ul>{''.join(summary)}</ul>
<table>{''.join(rows)}</table>
<p class="sub">Every comment comes from the engine's own search; the wording is filled into templates.</p>
</body></html>"""
    Path(path).write_text(page)


# ---------------------------------------------------------------------------- commands

def cmd_analyze(args, eng):
    games = read_pgn(args.pgn)
    headers, sans = games[args.game - 1]
    print(f"Analysing {headers.get('White', '?')} vs {headers.get('Black', '?')}: {len(sans)} plies at {args.time} ms")
    start, results = analyse_game(eng, headers, sans, args.time)
    out = Path(args.out)
    write_report(out.with_suffix(".html"), headers, start, results)
    write_annotated_pgn(out.with_suffix(".pgn"), headers, start, results)
    print(f"Wrote {out.with_suffix('.html')} and {out.with_suffix('.pgn')}")


def cmd_puzzles(args, eng):
    """A puzzle: after a mistake, the opponent has one move that wins and others don't."""
    puzzles = []
    for pgn in args.pgn:
        for gi, (headers, sans) in enumerate(read_pgn(pgn)):
            start, moves = start_of(headers), []
            prev = eng.analyse(start, moves, args.time, multipv=2)
            for ply, san in enumerate(sans):
                moves = moves + [san]
                cur = eng.analyse(start, moves, args.time, multipv=2)
                if not prev or not cur or len(cur) < 2:
                    prev = cur
                    continue
                loss = win_prob(prev[0][0]) - win_prob(-cur[0][0])
                best, second = cur[0][0], cur[1][0]
                if loss >= 0.25 and win_prob(best) >= 0.75 and win_prob(best) - win_prob(second) >= 0.3:
                    deep = eng.analyse(start, moves, args.time * 4, multipv=2)  # confirm deeper
                    if len(deep) >= 2 and win_prob(deep[0][0]) - win_prob(deep[1][0]) >= 0.3:
                        line = deep[0][1][:5 if len(deep[0][1]) >= 5 else len(deep[0][1]) // 2 * 2 + 1]
                        theme = tactic(eng.motifs(start, moves, line[0]))
                        puzzles.append({"fen": eng.fen(start, moves), "line": eng.san(start, moves, line),
                                        "theme": theme.split(":")[0] if theme else "",
                                        "score": deep[0][0], "source": f"{Path(pgn).name} game {gi + 1}",
                                        "after": san, "mate": abs(deep[0][0]) >= MATE - 500})
                        print(f"  puzzle {len(puzzles)}: {puzzles[-1]['fen']}  {' '.join(puzzles[-1]['line'])}")
                prev = cur
    out = Path(args.out)
    with open(out.with_suffix(".pgn"), "w") as f:
        for i, p in enumerate(puzzles, 1):
            side = "White" if p["fen"].split()[1] == "w" else "Black"
            theme = "mate" if p["mate"] else p["theme"] or "winning move"
            f.write(f'[Event "Puzzle {i}"]\n[Site "{p["source"]}"]\n[FEN "{p["fen"]}"]\n[SetUp "1"]\n'
                    f'[Result "*"]\n\n{{{side} to move: find the {theme} ({fmt(p["score"])}).}} '
                    f'{numbered(p["line"], p["fen"], 0)} *\n\n')
    cards = "".join(
        f'<div class="card"><h2>Puzzle {i}</h2><pre>{board_text(p["fen"])}</pre>'
        f'<p>{"White" if p["fen"].split()[1] == "w" else "Black"} to move. After {html.escape(p["after"])}, find the '
        f'{"mate" if p["mate"] else p["theme"] or "winning move"}.</p><details><summary>Solution</summary>'
        f'<p>{html.escape(numbered(p["line"], p["fen"], 0))} ({fmt(p["score"])})</p></details>'
        f'<p class="src">{html.escape(p["fen"])}</p></div>' for i, p in enumerate(puzzles, 1))
    Path(out.with_suffix(".html")).write_text(f"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Puzzles</title><style>
:root {{ --bg:#fafaf7; --fg:#222; --card:#fff; --border:#ddd; --muted:#777 }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#1b1b1d; --fg:#e8e8e8; --card:#242427; --border:#3a3a3e; --muted:#999 }} }}
body {{ background:var(--bg); color:var(--fg); font:15px/1.5 system-ui, sans-serif; max-width:820px; margin:0 auto; padding:16px }}
.card {{ background:var(--card); border:1px solid var(--border); border-radius:8px; padding:12px 16px; margin:12px 0 }}
pre {{ font-size:22px; line-height:1.15; margin:0 }} h2 {{ font-size:1.1em; margin:0 0 8px }} .src {{ color:var(--muted); font-size:.8em }}
</style></head><body><h1>Puzzles from the engine's games</h1>{cards or '<p>No puzzles found.</p>'}</body></html>""")
    print(f"{len(puzzles)} puzzles: {out.with_suffix('.pgn')}, {out.with_suffix('.html')}")


def board_text(fen):
    glyph = dict(zip("KQRBNPkqrbnp", "♔♕♖♗♘♙♚♛♜♝♞♟"))
    rows = []
    for r, rank in enumerate(fen.split()[0].split("/")):
        row = ""
        for c in rank:
            row += "·" * int(c) if c.isdigit() else glyph[c]
        rows.append(f"{8 - r} {row}")
    return "\n".join(rows) + "\n  abcdefgh"


def cmd_selfplay(args, options):
    """Games of the engine against itself from the start position, no book: Variety makes
    it choose among near-equal moves early on, so the games differ."""
    white = Engine(options=options + [("Variety", str(args.variety))])
    black = Engine(options=options + [("Variety", str(args.variety))])
    with open(args.out, "a") as f:
        for g in range(args.games):
            moves, result, seen = [], "*", Counter()
            for ply in range(400):
                fen = white.fen("startpos", moves)
                seen[" ".join(fen.split()[:4])] += 1
                if seen[" ".join(fen.split()[:4])] >= 3 or int(fen.split()[4]) >= 100 \
                        or not any(c in "pnbrqPNBRQ" for c in fen.split()[0]):
                    result = "1/2-1/2"  # repetition, 50-move rule, or bare kings
                    break
                eng = white if ply % 2 == 0 else black
                eng.set_position("startpos", moves)
                eng.send(f"go movetime {args.time}")
                out = eng.read_until("bestmove")
                best = out[-1].split()[1]
                score = next((l for l in reversed(out) if " score " in l), "")
                if best == "0000":
                    mate = "mate 0" in score
                    result = ("0-1" if ply % 2 == 0 else "1-0") if mate else "1/2-1/2"
                    break
                moves.append(best)
            sans = white.san("startpos", [], moves)
            if result == "*":
                result = "1/2-1/2"
            f.write(f'[Event "Self-play"]\n[Round "{g + 1}"]\n[White "engine"]\n[Black "engine"]\n'
                    f'[Result "{result}"]\n\n{wrap(numbered(sans, "startpos", 0))} {result}\n\n')
            print(f"  game {g + 1}: {' '.join(sans[:10])} ... {result}")
    white.quit()
    black.quit()


def cmd_openings(args):
    """The opening tree of a set of games: how often each line was played, how it scored."""
    tree = defaultdict(lambda: [0, 0.0])  # line -> [games, points for White]
    total = 0
    for pgn in args.pgn:
        for headers, sans in read_pgn(pgn):
            if "FEN" in headers:
                continue
            total += 1
            points = {"1-0": 1, "0-1": 0}.get(headers.get("Result"), 0.5)
            for n in range(1, min(args.plies, len(sans)) + 1):
                node = tree[tuple(sans[:n])]
                node[0] += 1
                node[1] += points

    def show(prefix, depth):
        children = sorted((k for k in tree if len(k) == depth + 1 and k[:depth] == prefix),
                          key=lambda k: -tree[k][0])
        for k in children:
            games, pts = tree[k]
            if games < args.min_games:
                continue
            print(f"{'  ' * depth}{numbered([k[-1]], 'startpos', depth)}  {games} games, "
                  f"White scores {pts / games:.0%}")
            show(k, depth + 1)

    print(f"{total} games")
    show((), 0)


def cmd_play(args, eng):
    """Play the engine in the terminal; after each of your moves it says how it was."""
    you = args.color[0]
    moves = []
    print("Moves in SAN (e4, Nf3, O-O). Commands: hint, threats, eval, undo, quit.")
    while True:
        eng.set_position("startpos", moves)
        eng.send("d")
        board = eng.read_until("Key:")
        print("\n".join(board[:-2]))
        ply = len(moves)
        if side_of("startpos", ply) != you:
            eng.set_position("startpos", moves)
            eng.send("setoption name ThinkAloud value true")
            eng.send(f"go movetime {args.time}")
            out = eng.read_until("bestmove")
            eng.send("setoption name ThinkAloud value false")
            best = out[-1].split()[1]
            if best == "0000":
                print("Game over.")
                return
            think = [l for l in out if "[think] decision" in l]
            print("Engine plays " + eng.san("startpos", moves, [best])[0]
                  + (": " + think[-1].split("decision: ", 1)[1] if think else ""))
            moves.append(best)
            continue
        cmd = input("your move> ").strip()
        if cmd == "quit":
            return
        if cmd == "undo":
            moves = moves[:-2]
            continue
        if cmd == "hint":
            lines = eng.analyse("startpos", moves, args.time)
            if lines:
                print(f"Hint: {eng.san('startpos', moves, lines[0][1][:1])[0]} ({fmt(lines[0][0])})")
            continue
        if cmd == "threats":
            print(eng.threats("startpos", moves) or "nothing")
            continue
        if cmd == "eval":
            print("\n".join(l for l in eng.query("startpos", moves, "eval", "") if l != "readyok"))
            continue
        if not legal_san(eng, moves, cmd):
            print("Not a legal move here.")
            continue
        before = eng.analyse("startpos", moves, args.time, multipv=2)
        after = eng.analyse("startpos", moves + [cmd], args.time)
        if before:
            loss = win_prob(before[0][0]) - win_prob(-after[0][0] if after else 0)
            best = eng.san("startpos", moves, before[0][1][:1])[0]
            if loss >= 0.05:
                verdict = "inaccuracy" if loss < 0.1 else "mistake" if loss < 0.2 else "blunder"
                print(f"That was {'an' if verdict[0] in 'aeiou' else 'a'} {verdict}: {best} was better ({fmt(before[0][0])}).")
            else:
                print("Good move." if cmd.rstrip("+#") != best.rstrip("+#") else "Best move!")
        moves.append(cmd)


def legal_san(eng, moves, san):
    before = eng.fen("startpos", moves)
    after = eng.fen("startpos", moves + [san])
    return before != after


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--option", action="append", default=[], metavar="NAME=VALUE")
    sub = ap.add_subparsers(dest="command", required=True)
    a = sub.add_parser("analyze")
    a.add_argument("pgn")
    a.add_argument("--game", type=int, default=1)
    a.add_argument("--time", type=int, default=500, help="ms per position")
    a.add_argument("--out", default="analysis")
    p = sub.add_parser("puzzles")
    p.add_argument("pgn", nargs="+")
    p.add_argument("--time", type=int, default=200)
    p.add_argument("--out", default="puzzles")
    s = sub.add_parser("selfplay")
    s.add_argument("--games", type=int, default=10)
    s.add_argument("--time", type=int, default=200)
    s.add_argument("--variety", type=int, default=20)
    s.add_argument("--out", default="selfplay.pgn")
    o = sub.add_parser("openings")
    o.add_argument("pgn", nargs="+")
    o.add_argument("--plies", type=int, default=6)
    o.add_argument("--min-games", type=int, default=2)
    pl = sub.add_parser("play")
    pl.add_argument("--color", choices=["white", "black"], default="white")
    pl.add_argument("--time", type=int, default=1000)
    args = ap.parse_args()
    options = [tuple(x.split("=", 1)) for x in args.option]

    if args.command == "openings":
        return cmd_openings(args)
    if args.command == "selfplay":
        return cmd_selfplay(args, options)
    eng = Engine(options=options)
    try:
        {"analyze": cmd_analyze, "puzzles": cmd_puzzles, "play": cmd_play}[args.command](args, eng)
    finally:
        eng.quit()


if __name__ == "__main__":
    main()
