# Chess engine: notes for coding agents

Read [docs/STATUS.md](docs/STATUS.md) first: what is committed, the uncommitted patch stack
in `build/steps/`, what is tested, and what is open. Usage is in [README.md](README.md).

Rules:
- Don't commit unless the owner asks. Never add AI/Claude attribution to commits or PRs.
- Every strength change: `make test`, `make bench`, then SPRT with `tools/sprt.py`. One
  change per test and per commit; H0 means revert.
- PeSTO stays pure (the fixed reference). The Learned/NNUE line trains only on its own
  self-play data; don't bypass the data-source checks.
- No opening book or tablebases in play.
- Teaching and style code stay outside the core search (coach over UCI, styles at the root).
- Never commit the Lichess token; never upgrade a Lichess account to a bot.
- Don't run CPU-heavy work while an SPRT is running (check `pgrep -fl sprt.py`).
