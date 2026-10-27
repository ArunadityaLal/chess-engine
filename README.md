# Classical Chess Engine

A UCI chess engine in C++20. Classical alpha-beta with a handcrafted evaluation first, NNUE later.
No opening book and no endgame tablebases: every move is the engine's own calculation.

## Build

```sh
make          # optimized build  -> build/release/engine
make debug    # ASan + UBSan     -> build/debug/engine
make run      # build release and start it
make test     # quick self-tests on the debug build (asserts + sanitizers, ~1 min)
make perft    # full gate on the release build: deep perft + deep minimax oracle (~1 min)
make bench    # bench signature: node count of a fixed search, for commit messages
make clean
```

## Testing a change (SPRT)

Every change that is supposed to make the engine stronger has to prove it in games:

1. Make the change, run `make test`.
2. `make bench`. If the node count didn't change, the change doesn't affect play: commit it.
3. `python3 tools/sprt.py` plays your uncommitted version against the last commit until the
   SPRT decides (books and both builds are handled automatically).
   - **H1 accepted**: it's stronger. Commit, with the bench and the printed summary in the message.
   - **H0 accepted**: it isn't. Revert, however good the idea seemed.
4. For removals or simplifications: `python3 tools/sprt.py --bounds simplify` (passes if not weaker).

Other uses:

```sh
python3 tools/sprt.py --base HEAD~1 --dev HEAD          # compare two commits
python3 tools/sprt.py --dev-option Eval=PeSTO --base-option Eval=Learned --games 200
python3 tools/sprt.py --tc 2+0.02                       # faster games (less reliable)
```

Defaults: 8+0.08 time control, 8moves_v3 openings (each played with both colors),
SPRT bounds [0, 5] normalized Elo, alpha = beta = 0.05. At this time control the machine
plays about 1,000 games an hour, so a 10,000-game test runs overnight. Runs are saved under
`build/sprt/runs/` and every result is appended to `build/sprt/history.txt`.

Requires `clang++` with C++20 support.

## Playing and watching

In En Croissant: Engines → Add new → Local, and pick `build/release/engine`.

Matches with [fastchess](https://github.com/Disservin/fastchess) (installed at `~/dev/tools/bin/fastchess`):

```sh
E=$PWD/build/release/engine
~/dev/tools/bin/fastchess -engine cmd=$E name=engine -engine cmd=$E name=random option.RandomMover=true \
    -each tc=5+0.05 -rounds 50 -games 2 -concurrency 6 -pgnout file=games.pgn
```

## UCI options

| Option | Default | |
|---|---|---|
| MultiPV | 1 | Report the N best lines |
| ThinkAloud | false | Explain each search iteration in plain language (`info string [think] ...`) |
| Move Overhead | 30 | Milliseconds kept in reserve per move for GUI lag |
| RandomMover | false | Play random legal moves (a test opponent) |
| Eval | PeSTO | `PeSTO` (borrowed knowledge, the reference) or `Learned` (self-taught line) |
| EvalFile | empty | Weights for the Learned eval; empty = material only (generation 0) |

## Two evaluations

- **PeSTO**: piece values and piece-square tables tuned by others on game positions. A fixed
  reference opponent; it never supplies training data.
- **Learned**: starts knowing only piece values (P 1, N 3, B 3, R 5, Q 9) and is tuned
  generation by generation on its own self-play games only. `evalparams` prints its current
  tables, so each generation's discoveries can be inspected. Generation 0 lost 40–0 to PeSTO.

## Commands

```
uci / isready / ucinewgame / setoption / stop / quit
position startpos [moves e2e4 e7e5 ...]
position fen <fen> [moves ...]
go [wtime N] [btime N] [winc N] [binc N] [movestogo N] [depth N] [nodes N] [movetime N] [infinite]

Teaching and debugging extensions:
d                 print the board, FEN and hash key
eval              breakdown of the evaluation: material, placement, phase, each piece
evalparams        the active evaluation's weights, in EvalFile format
threats [depth]   what the opponent would do if it were their move
perft <depth>     perft with per-move breakdown (also: go perft <depth>)
bench [depth]     fixed-depth search over fixed positions; node count is a fingerprint
test [full]       self-tests
```

## Roadmap

1. Board, move generation, perft (gate: standard perft suite matches)
2. UCI protocol
3. Baseline search: alpha-beta, iterative deepening, quiescence, PeSTO eval
4. Testing infrastructure: fastchess + SPRT
5. Search strength: TT, PVS, null move, LMR, pruning
6. Classical eval terms + Texel tuning; Learned line: self-play data generation and its own
   tuning, generation by generation
7. Freeze v1
8. NNUE, trained only on Learned-line games
9. Lazy SMP, Lichess bot

Teaching features alongside: coach app (game analysis, move classification, eval graph),
tactic detection, template explanations, puzzles from its own games.
