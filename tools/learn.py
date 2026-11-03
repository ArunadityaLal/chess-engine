#!/usr/bin/env python3
"""Train the Learned line, generation by generation, on its own games only.

Two stages, each a chain of generations:
  hand-crafted eval: evals/learned/genN.txt (gen0 is material only and has no file)
  NNUE (--nnue):     evals/nnue/netN.nnue, first trained on the hand-crafted stage's games

Each generation:
  1. self-play: the newest generation plays itself (datagen in several processes)
  2. training on the newest generations' positions, starting from the newest weights
     (Texel tuning for the eval terms, the NNUE trainer for networks)
  3. optionally a match: the new generation against the previous one and against PeSTO

Self-play data goes to build/data/learned/<generation>/. Nothing PeSTO played is ever
used: the tuner and the trainer both check every data file's source.

Examples:
  python3 tools/learn.py                          # one more hand-crafted generation
  python3 tools/learn.py --generations 5
  python3 tools/learn.py --nnue                   # first (or next) network
  python3 tools/learn.py --nnue --generations 3 --match 200
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "build" / "release" / "engine"
TRAINER = ROOT / "build" / "release" / "nnue-train"
EVALS = ROOT / "evals" / "learned"
NETS = ROOT / "evals" / "nnue"
DATA = ROOT / "build" / "data" / "learned"


class Gen:
    """One generation of the Learned line: 'gen3' (eval terms) or 'net2' (network)."""

    def __init__(self, stage, n):
        self.stage, self.n = stage, n
        self.name = f"{'gen' if stage == 'hce' else 'net'}{n}"

    @property
    def file(self):
        if self.stage == "hce":
            return EVALS / f"{self.name}.txt" if self.n else None
        return NETS / f"{self.name}.nnue"

    def options(self):
        """UCI options that make the engine play as this generation."""
        if self.stage == "hce":
            return [("Eval", "Learned")] + ([("EvalFile", self.file)] if self.file else [])
        return [("Eval", "NNUE"), ("NNUEFile", self.file)]

    def data(self):
        return DATA / self.name


def latest(stage):
    folder, pattern = (EVALS, r"gen(\d+)\.txt") if stage == "hce" else (NETS, r"net(\d+)\.nnue")
    found = [int(m.group(1)) for p in folder.glob("*") if (m := re.fullmatch(pattern, p.name))]
    return Gen(stage, max(found, default=0))


def all_data_gens():
    """Every generation with self-play data, oldest first."""
    gens = [Gen("hce", n) for n in range(latest("hce").n + 1)]
    gens += [Gen("nnue", n) for n in range(1, latest("nnue").n + 1)]
    return [g for g in gens if list(g.data().glob("*.bin"))]


def engine(commands, gen):
    """Runs engine commands as a generation; returns the output."""
    setup = [f"setoption name {k} value {v}" for k, v in gen.options()]
    text = "\n".join(setup + commands + ["quit"]) + "\n"
    return subprocess.run([str(ENGINE)], input=text, text=True, capture_output=True, check=True).stdout


def selfplay(gen, games, nodes, procs):
    out = gen.data()
    out.mkdir(parents=True, exist_ok=True)
    done = sum(p.stat().st_size for p in out.glob("*.bin")) // 32
    if done:
        print(f"  {gen.name} already has {done} positions; adding more")
    start = len(list(out.glob("*.bin")))
    base_seed = (0 if gen.stage == "hce" else 500) * 1_000_003 + gen.n * 1_000_003

    def part(i):
        return engine([f"datagen games {games // procs} nodes {nodes} seed {base_seed + start + i + 1} "
                       f"out {out / f'part{start + i}.bin'}"], gen)

    with ThreadPoolExecutor(procs) as pool:
        for log in pool.map(part, range(procs)):
            print("  " + log.strip().splitlines()[-1].replace("info string ", ""))


def data_files(gens):
    return [f for g in gens for f in sorted(g.data().glob("*.bin"))]


def tune(gen, files, args):
    target = Gen("hce", gen.n + 1)
    log = engine([f"tune epochs {args.epochs} lr {args.lr} lambda {args.lam} positions {args.positions} "
                  f"out {target.file} data {' '.join(str(f) for f in files)}"], gen)
    lines = [l.replace("info string tune: ", "") for l in log.splitlines() if "tune:" in l]
    for line in [l for l in lines if "K =" in l or " positions, " in l] + \
                [l for l in lines if l.startswith("epoch")][-1:]:
        print("  " + line)
    if not target.file.exists():
        sys.exit("tuning failed:\n" + log)
    return target


def train_net(previous, files, args):
    target = Gen("nnue", previous.n + 1 if previous.stage == "nnue" else 1)
    NETS.mkdir(parents=True, exist_ok=True)
    cmd = [str(TRAINER), "--data", *map(str, files), "--out", str(target.file),
           "--epochs", str(args.net_epochs), "--lr", str(args.net_lr), "--wdl", str(args.wdl),
           "--max-positions", str(args.net_positions)]
    if previous.stage == "nnue":
        cmd += ["--init", str(previous.file)]
    proc = subprocess.Popen(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for line in proc.stdout:
        if line.startswith(("epoch", "start", "saved")) or "training and" in line:
            print("  " + line.rstrip(), flush=True)
    if proc.wait() != 0 or not target.file.exists():
        sys.exit("network training failed")
    return target


def match(new, old, games, tc):
    fastchess = os.environ.get("FASTCHESS", str(Path.home() / "dev/tools/bin/fastchess"))
    book = ROOT / "books" / "8moves_v3.pgn"

    def player(gen):
        return ["-engine", f"cmd={ENGINE}", f"name={gen.name}"] + [f"option.{k}={v}" for k, v in gen.options()]

    pesto = ["-engine", f"cmd={ENGINE}", "name=PeSTO", "option.Eval=PeSTO"]
    for name, opponent in ((old.name, player(old)), ("PeSTO", pesto)):
        out = subprocess.run([fastchess, *player(new), *opponent, "-each", f"tc={tc}",
                              "-openings", f"file={book}", "format=pgn", "order=random", "-repeat",
                              "-games", "2", "-rounds", str(games // 2), "-concurrency",
                              str(max(1, (os.cpu_count() or 2) - 2))],
                             text=True, capture_output=True).stdout
        results = [l for l in out.splitlines() if l.startswith(("Elo:", "Games:"))]
        print(f"  {new.name} vs {name}: " + " | ".join(results[-2:]), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--generations", type=int, default=1)
    ap.add_argument("--nnue", action="store_true", help="train networks instead of eval terms")
    ap.add_argument("--games", type=int, default=20000, help="self-play games per generation")
    ap.add_argument("--nodes", type=int, default=5000, help="search nodes per move in self-play")
    ap.add_argument("--procs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--window", type=int, default=2, help="train on this many newest generations' data")
    ap.add_argument("--epochs", type=int, default=1000, help="Texel tuning epochs")
    ap.add_argument("--lr", type=float, default=1.0, help="Texel tuning step (centipawns)")
    ap.add_argument("--lambda", dest="lam", type=float, default=1.0,
                    help="Texel: weight of game results vs search scores in the target")
    ap.add_argument("--positions", type=int, default=3_000_000, help="Texel tuning sample (memory)")
    ap.add_argument("--net-epochs", type=int, default=30)
    ap.add_argument("--net-lr", type=float, default=0.001)
    ap.add_argument("--wdl", type=float, default=0.4, help="NNUE: weight of game results vs scores")
    ap.add_argument("--net-positions", type=int, default=30_000_000, help="NNUE training sample")
    ap.add_argument("--match", type=int, default=0, help="games vs previous gen and PeSTO (0: skip)")
    ap.add_argument("--tc", default="8+0.08")
    args = ap.parse_args()

    # Work with copies, so rebuilding meanwhile can't change a running training.
    global ENGINE, TRAINER
    subprocess.run(["make", "-j8"], cwd=ROOT, check=True, capture_output=True)
    subprocess.run(["make", "trainer"], cwd=ROOT, check=True, capture_output=True)
    for name in ("ENGINE", "TRAINER"):
        frozen = ROOT / "build" / f"learn-{globals()[name].name}"
        shutil.copy2(globals()[name], frozen)
        globals()[name] = frozen
    EVALS.mkdir(parents=True, exist_ok=True)

    for _ in range(args.generations):
        if not args.nnue:
            gen = latest("hce")
            print(f"{gen.name} -> gen{gen.n + 1}", flush=True)
            print(f"  self-play: {args.games} games at {args.nodes} nodes, {args.procs} processes")
            selfplay(gen, args.games, args.nodes, args.procs)
            gens = [Gen("hce", n) for n in range(max(0, gen.n - args.window + 1), gen.n + 1)]
            print(f"  tuning on {', '.join(g.name for g in gens)}", flush=True)
            new = tune(gen, data_files(gens), args)
        else:
            # The first network learns from the hand-crafted stage's games; later ones play
            # themselves first, like any generation.
            gen = latest("nnue") if latest("nnue").n else latest("hce")
            print(f"{gen.name} -> net{gen.n + 1 if gen.stage == 'nnue' else 1}", flush=True)
            if gen.stage == "nnue":
                print(f"  self-play: {args.games} games at {args.nodes} nodes, {args.procs} processes")
                selfplay(gen, args.games, args.nodes, args.procs)
            gens = all_data_gens()[-(args.window if gen.stage == "nnue" else 3):]
            print(f"  training on {', '.join(g.name for g in gens)}", flush=True)
            new = train_net(gen, data_files(gens), args)
        print(f"  wrote {new.file.relative_to(ROOT)}", flush=True)
        if args.match:
            match(new, gen, args.match, args.tc)


if __name__ == "__main__":
    main()
