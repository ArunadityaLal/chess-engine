# Classical Chess Engine

A UCI chess engine in C++20: alpha-beta search with a hand-crafted evaluation and an NNUE
trained only on the engine's own games. No opening book and no endgame tablebases: every
move is the engine's own calculation. Comes with a coach that analyses games and explains
moves, and a Lichess bot.

## Build

```sh
make          # optimized build  -> build/release/engine
make debug    # ASan + UBSan     -> build/debug/engine
make run      # build release and start it
make test     # quick self-tests on the debug build (asserts + sanitizers, ~1 min)
make perft    # full gate on the release build: deep perft + deep minimax oracle (~1 min)
make bench    # bench signature: node count of a fixed search, for commit messages
make trainer  # NNUE trainer     -> build/release/nnue-train
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
python3 tools/sprt.py --base path/to/engine             # against an engine binary
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

In the browser, with chess.com-style premoves (any number, played at zero clock time;
right-click cancels), synthesized move/capture/check and game-result sounds (toggleable),
automatic saving of every game to `games/`, and an Analyse button that runs the coach on
the finished game:

```sh
python3 tools/play/server.py          # opens http://localhost:8765
```

Matches with [fastchess](https://github.com/Disservin/fastchess) (installed at `~/dev/tools/bin/fastchess`):

```sh
E=$PWD/build/release/engine
~/dev/tools/bin/fastchess -engine cmd=$E name=engine -engine cmd=$E name=random option.RandomMover=true \
    -each tc=5+0.05 -rounds 50 -games 2 -concurrency 6 -pgnout file=games.pgn
```

## UCI options

| Option | Default | |
|---|---|---|
| Threads | 1 | Search threads (Lazy SMP: helpers share the transposition table) |
| Hash | 16 | Transposition table size in MB (cleared by `ucinewgame`) |
| MultiPV | 1 | Report the N best lines |
| ThinkAloud | false | Explain each search iteration in plain language (`info string [think] ...`) |
| Move Overhead | 30 | Milliseconds kept in reserve per move for GUI lag |
| RandomMover | false | Play random legal moves (a test opponent) |
| Eval | PeSTO | `PeSTO` (borrowed knowledge, the reference), `Learned` or `NNUE` (the self-taught line) |
| EvalFile | empty | Weights for the Learned eval; empty = material only (generation 0) |
| NNUEFile | empty | Network for `Eval=NNUE` (from `tools/learn.py --nnue`) |
| Contempt | 0 | Centipawns: a draw counts as this much worse than level for the engine |
| Style | Objective | `Creative` (when winning, prefer a sound sacrifice) or `Practical` (when level or worse, prefer moves that leave the opponent few good replies) |
| Variety | 0 | Centipawns: in the first 20 plies, pick randomly among moves this close to the best |

## Evaluations

- **PeSTO**: piece values and piece-square tables tuned by others on game positions. A fixed
  reference opponent; it never supplies training data.
- **Learned**: piece values, placement tables and classical terms (mobility, passed pawns,
  pawn structure, bishop pair, rooks on open files, outposts, king attack and pawn shield,
  threats, hanging pieces, tempo). It starts knowing only piece values (P 1, N 3, B 3, R 5,
  Q 9, every other weight zero) and is tuned generation by generation on its own self-play
  games only. `evalparams` prints its weights and `eval` shows each category, so each
  generation's discoveries can be inspected. Generation 0 lost 40–0 to PeSTO.
- **NNUE**: the Learned line's next stage, a network (768 → 256 × 2 → 1, SCReLU) first
  trained on the hand-crafted stage's games, then on its own. `eval` shows what each piece is
  worth where it stands.

The Learned line never sees PeSTO's games: every data file records which evaluation played
it, and the tuner and the trainer refuse anything else.

## Training the Learned line

```sh
python3 tools/learn.py                     # one generation: self-play, then Texel tuning
python3 tools/learn.py --generations 5     # evals/learned/gen1.txt ... gen5.txt
python3 tools/learn.py --nnue              # first network, from the hand-crafted games
python3 tools/learn.py --nnue --generations 3 --match 200   # with a match per generation
```

Each generation plays itself (20,000 games at 5,000 nodes a move by default, a few minutes
on 8 cores), then the next generation is fitted to those games, starting from the current
weights. Data goes to `build/data/learned/<generation>/` (32 bytes a position), weights
to `evals/`. The pieces can also be run by hand:

```
setoption name Eval value Learned
setoption name EvalFile value evals/learned/gen3.txt
datagen games 1000 nodes 5000 seed 1 out gen3.bin       # self-play training data
tune epochs 1000 out gen4.txt data gen3.bin              # Texel tuning
datadump gen3.bin 5                                      # look at the data

build/release/nnue-train --data gen3.bin --out net.nnue --epochs 30
```

## Coach

`tools/coach/coach.py` (Python standard library only) works on top of the engine:

```sh
python3 tools/coach/coach.py analyze game.pgn --time 500     # analysis.html + analysis.pgn
python3 tools/coach/coach.py puzzles games.pgn               # puzzles.pgn + puzzles.html
python3 tools/coach/coach.py selfplay --games 50             # book-free self-play games
python3 tools/coach/coach.py openings selfplay.pgn --plies 8 # what openings it discovered
python3 tools/coach/coach.py play --color black              # play it, with feedback
```

- **analyze** classifies every move by how much it changed the winning chances (best,
  excellent, good, inaccuracy, mistake, blunder; brilliant for a sound sacrifice, "only
  move" when everything else is much worse), explains the bad ones ("loses a knight: ...",
  "allows mate in 3", "better was ..."), and draws the evaluation graph. The annotated PGN
  opens in any GUI.
- **puzzles** finds positions after a mistake where exactly one move wins.
- **play** tells you after each move how it was; `hint`, `threats`, `eval`, `undo`.

Every comment comes from engine facts (searches, material along the lines, tactical
motifs from `motifs`); the wording is filled into templates.

## Lichess bot

`tools/lichess_bot.py` plays on Lichess with this engine (standard library only). It needs
a separate Lichess account upgraded to a bot account and an API token with the `bot:play`
scope; see the notes at the top of the script.

```sh
export LICHESS_TOKEN=...
python3 tools/lichess_bot.py --option Threads=2 --min-base 60 --max-base 900
```

## Commands

```
uci / isready / ucinewgame / setoption / stop / quit
position startpos [moves e2e4 e7e5 ...]   (moves in UCI or SAN: e4 e5 Nf3)
position fen <fen> [moves ...]
go [wtime N] [btime N] [winc N] [binc N] [movestogo N] [depth N] [nodes N] [movetime N] [infinite]

Teaching and debugging extensions:
d                 print the board, FEN and hash key
eval              breakdown of the evaluation: material, placement, phase, each piece
evalparams        the active evaluation's weights, in EvalFile format
threats [depth]   what the opponent would do if it were their move
motifs <move>     tactical patterns of a move: fork, pin, skewer, discovered attack, mate...
fen               the current position's FEN
sanline <moves>   a line of UCI moves in SAN, from the current position
perft <depth>     perft with per-move breakdown (also: go perft <depth>)
bench [depth]     fixed-depth search (default 13) over fixed positions; node count is a fingerprint
test [full]       self-tests

Training:
datagen [games N] [nodes N] [randomplies N] [seed N] out <file>   self-play data
tune [epochs N] [lr X] [lambda X] [positions N] out <file> data <files...>
datadump <file> [N]                                              print training positions
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
tactic detection, template explanations, puzzles from its own games. Playing styles
(contempt, Creative, Practical) as a layer at the root, outside the tested search.
