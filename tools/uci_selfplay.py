#!/usr/bin/env python3
"""UCI smoke test: plays engine-vs-engine games over stdin/stdout pipes, the way a GUI
does, and fails on illegal moves, crashes, hangs, protocol errors or time losses.

Move legality and game state come from a third engine process acting as referee (its
move generator is perft-verified). Phase 4 replaces this with fastchess.

Usage: python3 tools/uci_selfplay.py [--games N] [--engine PATH] [--tc MS] [--inc MS]
"""
import argparse
import collections
import queue
import random
import re
import subprocess
import sys
import threading
import time

DIVIDE_LINE = re.compile(r"^([a-h][1-8][a-h][1-8][nbrq]?): 1$")


class ProtocolError(Exception):
    pass


class Engine:
    def __init__(self, path, name):
        self.name = name
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1
        )
        self.lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)  # EOF

    def send(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except BrokenPipeError:
            raise ProtocolError(f"{self.name}: engine closed its input (crashed?)")

    def expect(self, prefix, timeout=5.0):
        """Read until a line starting with prefix. Returns (that line, all lines read)."""
        deadline = time.monotonic() + timeout
        seen = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError(f"{self.name}: no '{prefix}' within {timeout:.1f}s")
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if line is None:
                raise ProtocolError(f"{self.name}: process exited, code {self.proc.poll()}")
            if "illegal move" in line or "unknown command" in line or "invalid FEN" in line:
                raise ProtocolError(f"{self.name}: {line}")
            seen.append(line)
            if line.startswith(prefix):
                return line, seen

    def handshake(self):
        self.send("uci")
        self.expect("uciok")
        self.send("isready")
        self.expect("readyok")

    def quit(self):
        self.send("quit")
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise ProtocolError(f"{self.name}: did not exit after quit")
        if self.proc.returncode != 0:
            raise ProtocolError(f"{self.name}: exit code {self.proc.returncode}")


def position_cmd(moves):
    return "position startpos" + (" moves " + " ".join(moves) if moves else "")


class Referee:
    def __init__(self, path):
        self.engine = Engine(path, "referee")
        self.engine.handshake()

    def state(self, moves):
        """(legal moves, FEN) after the given moves."""
        self.engine.send(position_cmd(moves))
        self.engine.send("d")
        fen_line, _ = self.engine.expect("FEN: ")
        self.engine.send("perft 1")
        _, seen = self.engine.expect("NPS:")
        legal = {m.group(1) for m in map(DIVIDE_LINE.match, seen) if m}
        return legal, fen_line[5:]


def insufficient_material(fen):
    pieces = [c for c in fen.split()[0] if c.isalpha() and c not in "Kk"]
    return not pieces or (len(pieces) == 1 and pieces[0] in "NBnb")


def play_game(path, referee, tc, inc, max_plies):
    engines = [Engine(path, "white"), Engine(path, "black")]
    for e in engines:
        e.handshake()
        e.send("ucinewgame")
        e.send("isready")
        e.expect("readyok")

    moves, clock = [], [tc, tc]
    repetitions = collections.Counter()
    while True:
        side = len(moves) % 2
        legal, fen = referee.state(moves)
        repetitions[" ".join(fen.split()[:4])] += 1

        if not legal:
            result = "checkmate/stalemate"
        elif repetitions[" ".join(fen.split()[:4])] >= 3:
            result = "threefold"
        elif int(fen.split()[4]) >= 100:
            result = "50-move rule"
        elif insufficient_material(fen):
            result = "insufficient material"
        elif len(moves) >= max_plies:
            result = "ply cap"
        else:
            result = None
        if result:
            break

        engine = engines[side]
        engine.send(position_cmd(moves))
        if random.random() < 0.1:
            # Analysis-style: go infinite, isready mid-search, then stop.
            engine.send("go infinite")
            time.sleep(0.005)
            engine.send("isready")
            _, seen = engine.expect("readyok")
            if any(line.startswith("bestmove") for line in seen):
                raise ProtocolError(f"{engine.name}: bestmove before stop in go infinite")
            engine.send("stop")
            line, _ = engine.expect("bestmove")
        else:
            start = time.monotonic()
            engine.send(f"go wtime {clock[0]} btime {clock[1]} winc {inc} binc {inc}")
            line, _ = engine.expect("bestmove", timeout=clock[side] / 1000 + 1)
            clock[side] -= int((time.monotonic() - start) * 1000)
            if clock[side] < 0:
                raise ProtocolError(f"{engine.name}: lost on time at ply {len(moves)}")
            clock[side] += inc

        move = line.split()[1]
        if move not in legal:
            raise ProtocolError(f"{engine.name}: illegal move {move} at ply {len(moves)}")
        moves.append(move)

    for e in engines:
        e.quit()
    return result, len(moves)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="./build/release/engine")
    ap.add_argument("--games", type=int, default=100)
    ap.add_argument("--tc", type=int, default=10000, help="ms per side")
    ap.add_argument("--inc", type=int, default=100, help="ms increment")
    ap.add_argument("--max-plies", type=int, default=600)
    args = ap.parse_args()

    referee = Referee(args.engine)
    results = collections.Counter()
    plies = 0
    start = time.monotonic()
    for g in range(1, args.games + 1):
        try:
            result, n = play_game(args.engine, referee, args.tc, args.inc, args.max_plies)
        except ProtocolError as err:
            print(f"game {g}: FAILED: {err}")
            return 1
        results[result] += 1
        plies += n
        if sys.stdout.isatty():
            print(f"\rgame {g}/{args.games}: {result} after {n} plies".ljust(60), end="", flush=True)
    referee.engine.quit()

    print(f"\n{args.games} games, {plies} moves, no illegal moves, crashes or time losses "
          f"({time.monotonic() - start:.0f} s)")
    for result, count in results.most_common():
        print(f"  {result}: {count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
