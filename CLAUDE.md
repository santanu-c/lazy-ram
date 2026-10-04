# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

lazy-ram detects whether **main memory drops into a power-saving (deep-sleep) state when idle**. It times hash-map lookups, each a real DRAM access into a pre-sized table of over 1 GB with a new key every time, after a configurable busy-wait gap (`sleep_usec`). Latency that rises with the gap means memory is sleeping. The busy-wait keeps the core awake on purpose, so slowdowns are attributable to the memory side. Don't change it to a real sleep without asking. It has two implementations that must **stay behaviourally in step**: the same arguments, env vars, loop structure and output format. When you change one, mirror the change in the other.

- C++: `src/main.cpp` (benchmark) and `src/zprobes.cpp` + `include/lazyram/zprobes.h` (timing probes)
- Python: `python/lazy_ram.py` (CPython 3.12 + Numba)

## Commands

```sh
make                        # build/lazy_ram (C++20, -O3); `make debug` for -O0 -g in build/debug/
make run ARGS="0 1"         # ./build/lazy_ram <sleep_usec> <scrub_cache>
make venv                   # .venv with python3.12 + numpy/numba (override PYTHON=...)
make run-python ARGS="0 1"
make clean / make distclean # distclean also removes .venv

# Quick runs: a full run is 1M batches of 100 lookups
LAZYRAM_BATCHES=5 ./build/lazy_ram 0 0
LAZYRAM_BATCHES=3 LAZYRAM_COUNT=200000 .venv/bin/python python/lazy_ram.py 0 1
```

There are no tests, linters or CI builds. `.github/workflows/` holds only the Claude Code bot (`@claude`, `claude.yml`) and the automatic PR review (`claude-code-review.yml`). To check a change, build with no warnings and compare a short run of both implementations.

If `python3.12` isn't on PATH, use `uv python install 3.12` and then `make venv PYTHON="$(uv python find 3.12)"`.

## Architecture

- **Timing (zprobe).** Samples are CPU cycles read from RDTSC, so the code is **x86_64-only** (`#error` on anything else; Apple Silicon is out).
  - C++ uses inline-asm `CPUID`+`RDTSC` to start and `RDTSCP`+`CPUID` to stop.
  - Python emits the **same instructions** through llvmlite `InlineAsm` intrinsics (`rdtsc_start`/`rdtsc_stop`). This needs `llvm.initialize_native_asmparser()`; without it, LLVM aborts with "Inline asm not supported by this streamer". The unfenced `rdtsc()` (`llvm.readcyclecounter`) is only for the busy-wait.
  - Don't switch the start read to RDTSCP. It doesn't stop later instructions from starting before the read; README "How the timer works" explains why.
  - `calibrate()` must run first in both. It measures the probe's own overhead (`self_adjustment`, which `zstop` subtracts) and works out `nanos_per_cycle` against wall time over a **2 s sleep**.
  - `zlog` collects samples per batch id. `zpercentile_ns(id, p)` reads them back in ns (p = 100 gives the max), and `zreset` drops them. The older `zdump*` printers are kept as library helpers; the benchmark doesn't use them.
- **Benchmark flow.** For each batch `j`, there are 100 lookups of key `to_string(k + j*100)` in a hash map of `Value { char foo[20]; int v; }`. Before each lookup the code optionally busy-waits (`mysleep`) and optionally runs `blow_cache()` (random-fills, then sums, an int array of `LAZYRAM_COUNT` elements). Only the lookup itself sits between `zstart`/`zstop`. A missing key inserts a default `Value` inside the timed region, as `dense_hash_map::operator[]` does. After each batch, `main` reads `zpercentile_ns(id, 99)` and `zpercentile_ns(id, 100)`, prints the batch line, then calls `zreset(id)`.
- **Output.** `main` owns the report: a Settings block that names where each value came from (arg 1/arg 2/env var), the calibration result, one `Batch / p99` line per batch, then a Summary (median batch p99, worst batch p99, slowest single lookup) and a Checksum. Red is used only when stdout is a TTY and `NO_COLOR` is unset. Arguments are validated (`sleep_usec` >= 0, `scrub_cache` 0/1), and `-h`/`--help` prints usage with examples. **README.md's "Reading the output" section documents this format; update it whenever the format changes.**
- **C++ hash map.** `google::dense_hash_map<std::string, Value, Fnv1a64>` from the vendored `third_party/sparsehash` (included with `-isystem`). It requires `set_empty_key("")` before use.
- **Python/Numba constraints.**
  - Everything inside the timed loop (`run_batch`, `blow_cache`, `mysleep`) must stay `@njit(cache=True)`-compatible.
  - The hash map is a `numba.typed.Dict[unicode, NamedTuple]`. It can't take a custom hash, so `fnv1a` is kept only for parity.
  - `main()` does a warm-up `run_batch` call so JIT compilation never lands in the samples.
  - The outer batch loop and printing stay in plain Python.
- `python/requirements.txt` pins `numba<0.63` on Intel macOS, because later numba/llvmlite releases ship no Intel-mac wheels. Don't drop the pin.

## Git workflow

- **`main` is protected.** Never push to it directly: create a branch, push, and open a PR. A PR needs no approving review, but the `claude-review` check must pass before merging. Force-pushes to `main` and deleting it are blocked. Admins can bypass the rules, so don't rely on that.
- Both workflows depend on the Claude GitHub App being installed on the repo. If `claude-review` fails with "401 … Claude Code is not installed on this repository", the fix is reinstalling the app at https://github.com/apps/claude, not a code change.
- After a PR merges: `git checkout main && git pull` before starting the next branch.
- GitHub Pages is **not** enabled. Turning it on publishes a public site, which is the user's call; don't do it unprompted.

## Conventions

- `third_party/` is vendored, unmodified upstream code. Don't edit it.
- `docs/workflow.png` is a rendered image of the run flow, components and timed region, and the README links to it. GitHub shows `.html` as source, which is why it's a PNG. The hand-drawn SVG/HTML source isn't kept in the repo (see PLAN item 12). When the flow, file layout or timer changes, redraw it, or flag that it's out of date. To render an HTML page to PNG: serve it with `python3 -m http.server`, then run headless Chrome with `--force-device-scale-factor=2 --window-size=900,<page height> --screenshot=...`.
- Probe API names are lower-case and identical in both languages: `zstart`, `zstop`, `zlog`, `zpercentile_ns`, `zreset` (plus `zdump_percentiles`/`zdumplite`).
- `FINDINGS.lazy-ram.md` is a local, git-ignored analysis of the original program. Don't commit it.
- `PLAN.lazy-ram.md` is the roadmap for future work. Check it before starting a change, and update its status when an item is done.
