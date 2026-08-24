# Contributing

## Dev setup and tests

Each component has its own environment. Run the suite for whatever you
touched before opening a PR; run all four if the change is cross-cutting.

```sh
# pi-server
cd pi-server && python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt && pip install pytest
python -m pytest -q

# relay
cd relay && python3 -m venv .venv && . .venv/bin/activate
pip install pytest
python -m pytest -q

# integration (real IPP + sync API + relay servers, a fake CUPS `lp`,
# and tools/simulate_x4.py). Run from repo root with pi-server's venv active.
python -m pytest tests/integration -q

# firmware host tests (pure-logic modules, no board needed)
cd firmware/test && cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

For a real X4/Pi setup (not just tests), see `docs/setup-pi.md` and
`docs/setup-x4.md`.

## Code style

- **pi-server**: pure stdlib (`http.server`, `sqlite3`, `ssl`) except where
  a dependency does something the stdlib genuinely can't — currently
  `PyMuPDF` and `Pillow` for PDF→XTC conversion (see
  `pi-server/requirements.txt`). Don't reach for a new third-party
  dependency for something the stdlib already covers; this runs on a Pi
  Zero W with 512MB shared with CUPS and avahi, so keep the footprint
  small.
- **relay**: pure stdlib, no exceptions — `relay/requirements.txt` is
  intentionally empty. It should run on whatever small VPS or container
  someone points it at with no install step beyond a Python interpreter.
- **firmware**: built on the FreeInk SDK; no raw display-controller or GPIO
  code in `firmware/src/` — that belongs in the SDK, not here. Follow the
  existing `PersistableStore`-style pattern (atomic JSON write-then-rename)
  for anything persisted to SD — see `firmware/src/store/`.
- Match the file you're editing. If you're unsure why something's built a
  particular way, `docs/architecture.md` explains the reasoning and what
  each component is/isn't supposed to do.

## PR expectations

- Tests pass for whatever you touched (see above). If you can't run a
  suite (e.g. no hardware for a firmware change beyond the host tests),
  say so in the PR description rather than skipping silently.
- Keep PRs small and focused — one change, one PR. Split unrelated fixes
  out separately.
- If a change affects the wire protocol or trust boundaries, update
  `docs/protocol.md` or `docs/security.md` in the same PR — code and docs
  drifting apart is worse than a slightly larger diff.
- Use the PR template; check off the test suites you actually ran.
