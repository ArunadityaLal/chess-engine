#!/usr/bin/env python3
"""Test whether one version of the engine is stronger than another.

Builds both versions, compares their bench signatures, then plays them against each
other with fastchess until an SPRT (sequential probability ratio test) decides:
  H1 accepted: dev is stronger by at least elo0..elo1  -> keep the change
  H0 accepted: dev is not stronger                     -> drop it
Every opening from the book is played twice with colors reversed, so a lopsided
opening can't favor either side.

Examples:
  python3 tools/sprt.py                          # uncommitted changes vs last commit
  python3 tools/sprt.py --base HEAD~1 --dev HEAD # last commit vs the one before
  python3 tools/sprt.py --bounds simplify        # removal: dev must not be weaker
  python3 tools/sprt.py --dev-option Eval=PeSTO --base-option Eval=Learned --games 200
"""
import argparse
import datetime
import os
import re
import shutil
import signal
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "sprt"
BOOKS = ROOT / "books"
BOOK_URL = "https://raw.githubusercontent.com/official-stockfish/books/master/{}.zip"
DEFAULT_BOOK = "8moves_v3.pgn"
BOUNDS = {"gainer": (0.0, 5.0), "simplify": (-5.0, 0.0)}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout


def find_fastchess():
    for candidate in (os.environ.get("FASTCHESS"), shutil.which("fastchess"),
                      str(Path.home() / "dev/tools/bin/fastchess")):
        if candidate and Path(candidate).is_file():
            return candidate
    sys.exit("fastchess not found: set FASTCHESS or put it on PATH")


def get_book(name):
    path = Path(name) if Path(name).is_file() else BOOKS / name
    if path.is_file():
        return path
    print(f"Downloading opening book {name} ...")
    BOOKS.mkdir(exist_ok=True)
    archive = BOOKS / (name + ".zip")
    urllib.request.urlretrieve(BOOK_URL.format(name), archive)
    with zipfile.ZipFile(archive) as z:
        z.extract(name, BOOKS)
    archive.unlink()
    return path


def build(ref):
    """Engine binary for a git ref, or for the current files when ref is 'worktree'."""
    engines = WORK / "engines"
    engines.mkdir(parents=True, exist_ok=True)

    if ref == "worktree":
        run(["make", "-j8"], cwd=ROOT)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        binary = engines / f"worktree-{stamp}"  # a snapshot: later edits can't change it
        shutil.copy2(ROOT / "build/release/engine", binary)
        return "worktree", binary

    sha = run(["git", "rev-parse", "--short", f"{ref}^{{commit}}"], cwd=ROOT).strip()
    binary = engines / sha
    if not binary.exists():
        print(f"Building {ref} ({sha}) ...")
        src = WORK / "src" / sha
        shutil.rmtree(src, ignore_errors=True)
        src.mkdir(parents=True)
        archive = subprocess.Popen(["git", "archive", sha], cwd=ROOT, stdout=subprocess.PIPE)
        subprocess.run(["tar", "-x", "-C", src], stdin=archive.stdout, check=True)
        archive.wait()
        run(["make", "-j8"], cwd=src)
        shutil.copy2(src / "build/release/engine", binary)
        shutil.rmtree(src)
    return sha, binary


def bench(binary):
    out = run([str(binary), "bench"])
    return int(re.search(r"Nodes searched: (\d+)", out).group(1))


def score_from_pgn(pgn, dev_name):
    """Wins, losses and draws for dev, counted from the saved games."""
    wins = losses = draws = 0
    if not pgn.exists():
        return wins, losses, draws
    for game in pgn.read_text().split("[Event ")[1:]:
        white = re.search(r'\[White "([^"]+)"', game)
        result = re.search(r'\[Result "([^"]+)"', game)
        if not white or not result or result.group(1) == "*":
            continue
        if result.group(1) == "1/2-1/2":
            draws += 1
        elif (result.group(1) == "1-0") == (white.group(1) == dev_name):
            wins += 1
        else:
            losses += 1
    return wins, losses, draws


def parse_bounds(text):
    if text in BOUNDS:
        return BOUNDS[text]
    lo, hi = (float(x) for x in text.split(","))
    return lo, hi


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dev", default="worktree", help="git ref, or 'worktree' (default)")
    ap.add_argument("--base", default="HEAD", help="git ref (default HEAD)")
    ap.add_argument("--dev-option", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--base-option", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--tc", default="8+0.08", help="seconds+increment (default 8+0.08)")
    ap.add_argument("--bounds", default="gainer",
                    help="'gainer' (0,5), 'simplify' (-5,0), or 'lo,hi' in normalized Elo")
    ap.add_argument("--games", type=int, help="play a fixed number of games instead of SPRT")
    ap.add_argument("--concurrency", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--book", default=DEFAULT_BOOK)
    ap.add_argument("--force", action="store_true", help="run even if the bench signatures match")
    args = ap.parse_args()

    fastchess = find_fastchess()
    book = get_book(args.book)
    dev_label, dev_bin = build(args.dev)
    base_label, base_bin = build(args.base)
    dev_bench, base_bench = bench(dev_bin), bench(base_bin)
    print(f"dev  {dev_label:>12}  bench {dev_bench}")
    print(f"base {base_label:>12}  bench {base_bench}")

    if dev_bench == base_bench and args.dev_option == args.base_option and not args.force:
        print("\nIdentical bench signatures: the change doesn't alter how the engine searches,\n"
              "so there is nothing to test. (Use --force to play anyway.)")
        return 0

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = WORK / "runs" / f"{stamp}-{base_label}-vs-{dev_label}"
    run_dir.mkdir(parents=True)

    def engine_args(label, binary, options):
        return ["-engine", f"cmd={binary}", f"name={label}"] + [f"option.{o}" for o in options]

    dev_name, base_name = f"dev-{dev_label}", f"base-{base_label}"
    cmd = [fastchess,
           *engine_args(dev_name, dev_bin, args.dev_option),
           *engine_args(base_name, base_bin, args.base_option),
           "-each", f"tc={args.tc}",
           "-openings", f"file={book}", f"format={'pgn' if book.suffix == '.pgn' else 'epd'}",
           "order=random",
           "-repeat", "-games", "2",
           "-concurrency", str(args.concurrency),
           "-recover", "-ratinginterval", "20",
           "-pgnout", f"file={run_dir / 'games.pgn'}"]
    if args.games:
        cmd += ["-rounds", str((args.games + 1) // 2)]
        test = f"fixed {args.games} games"
    else:
        lo, hi = parse_bounds(args.bounds)
        cmd += ["-rounds", "50000", "-sprt", f"elo0={lo}", f"elo1={hi}", "alpha=0.05", "beta=0.05"]
        test = f"SPRT [{lo:g}, {hi:g}]"

    print(f"\n{test}, tc {args.tc}, book {book.name}, concurrency {args.concurrency}")
    print(f"Games and log: {run_dir}\n")

    log = open(run_dir / "fastchess.log", "w")
    proc = subprocess.Popen(cmd, cwd=run_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    report = []
    try:
        for line in proc.stdout:
            log.write(line)
            if line.startswith(("Started game", "Finished game")):
                continue  # one line per game is noise on the console; it's in the log
            print(line, end="", flush=True)
            if line.startswith("Results of"):
                report = [line.rstrip()]
            elif report and line.strip() and not line.startswith(("---", "Finished match")):
                report.append(line.rstrip())
    except KeyboardInterrupt:
        print("\nStopping (results so far below) ...")
        proc.send_signal(signal.SIGINT)
    proc.wait()
    log.close()

    if not report:  # stopped before fastchess's first report
        w, l, d = score_from_pgn(run_dir / "games.pgn", dev_name)
        report = ["", f"Games: {w + l + d}, Wins: {w}, Losses: {l}, Draws: {d} (stopped early)"]

    summary = [f"{test}, tc {args.tc}, book {book.name}",
               f"dev:  {dev_label} (bench {dev_bench})" + "".join(f" {o}" for o in args.dev_option),
               f"base: {base_label} (bench {base_bench})" + "".join(f" {o}" for o in args.base_option),
               *report[1:]]
    (run_dir / "summary.txt").write_text("\n".join(summary) + "\n")
    with open(WORK / "history.txt", "a") as history:
        history.write(f"[{stamp}]\n" + "\n".join(summary) + "\n\n")

    print("\n=== Summary (also in summary.txt; paste into the commit message) ===")
    print("\n".join(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
