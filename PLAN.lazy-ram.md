# PLAN: lazy-ram

**Purpose:** detect whether main memory drops into a power-saving (deep-sleep) state when idle. Lookup latency should stay flat as the idle gap (`sleep_usec`) grows; a rise means memory is sleeping. Every item below should serve that goal.

The roadmap and working rules for future code changes to lazy-ram. Read it together with `CLAUDE.md` (how the code works) and `FINDINGS.lazy-ram.md` (what the original program got wrong, a local-only file). When an item ships, mark it **Done** with the date and move it to the "Completed" section at the end.

## Current state (2026-10-04)

- C++20 benchmark (`src/main.cpp`, `src/zprobes.cpp`, `include/lazyram/zprobes.h`) built by a portable `Makefile`. It is clean under Apple clang 16 (Intel Mac); GCC 15 is untested.
- Python 3.12 + Numba version (`python/lazy_ram.py`) with the same arguments, environment variables and report format. It is about 2.3× slower per lookup than C++.
- Report: Settings → calibration → one p99 line per batch → Summary → Checksum. Documented in `README.md`.
- No tests and no CI build. `.github/workflows/` only runs the Claude bot and the PR review.

## Rules for every change

1. **Parity:** any change to behaviour, arguments, environment variables or output goes into **both** `src/main.cpp` and `python/lazy_ram.py` in the same change.
2. **The output format is documented.** If the report changes, update README.md's "Reading the output" section (the sample block and the table) and the `--help` text in both languages.
3. **Timed region:** only the lookup itself goes between `zstart`/`zstop`. Never add I/O, allocation outside the map, or logging inside it.
4. **Platform:** x86_64 Linux (GCC 15+) and Intel macOS (Apple clang). Keep Makefile flags portable, and keep the Intel-mac `numba<0.63` pin.
5. **Don't edit `third_party/`.**
6. **Verify before calling a change done:**
   ```sh
   make clean && make 2>&1 | grep -i warn        # expect nothing
   make debug                                     # still builds
   LAZYRAM_BATCHES=5 ./build/lazy_ram 0 0
   LAZYRAM_BATCHES=3 LAZYRAM_COUNT=200000 ./build/lazy_ram 10 1
   LAZYRAM_BATCHES=5 .venv/bin/python python/lazy_ram.py 0 0    # same layout as C++
   ./build/lazy_ram 0 2; echo $?                  # error + usage, exit 1
   ```
7. **Keep it standalone:** never reference `hpc-benchmarks` or `LazyRAM2`.

## Backlog (in priority order)

### P1: Confidence

**1. CI build on GCC 15.** Add a `.github/workflows/build.yml` job that runs in the `gcc:15` container: `make`, `make debug`, then a short run of each argument combination. Add a second job that runs `make venv` + a short Python run on `ubuntu-latest` with Python 3.12.
*Done when:* both jobs pass on a PR, and the README "Requirements" no longer needs the GCC-15-untested caveat.

**2. `make check` smoke test.** Add a script (shell or Python) that runs both implementations with `LAZYRAM_BATCHES=3` and checks:
- exit codes;
- that the section headers and Settings lines match, ignoring the numbers;
- that bad arguments exit 1.

CI from item 1 should run it.
*Done when:* `make check` passes locally, and a deliberate format change in only one language makes it fail.

### P2: Measurement quality

**2a. Built-in idle-gap sweep (the core use case).** Add `--sweep 0,10,100,1000,10000`, or a `make sweep` target. It runs the benchmark at each gap after a single calibration and prints one comparison table (gap → typical p99, worst p99, slowest lookup, and the change from the 0 µs baseline). It should flag the first gap where the typical p99 rises more than a set threshold (e.g. 20%) as "memory sleep detected above N µs". Both languages; document it in the README "Detecting memory sleep" section.
*Done when:* one command produces the README's example table and the verdict line.

**2b. Real-sleep mode for comparison.** An option, e.g. `sleep_mode=busy|os`, that uses `nanosleep` instead of busy-waiting, so users can compare memory-only sleep (busy) against core + memory sleep (os), which is what the original `usleep` measured. Busy stays the default.

**3. Choose what a "lookup" measures.** Today the map starts empty, so every timed lookup is an insert-on-miss (see FINDINGS #9). Add `LAZYRAM_PREFILL=1`, or a third argument, to insert all keys before timing so lookups hit existing entries. Default to today's behaviour, show the mode in Settings, and document it in the README.
*Done when:* both modes run in both languages, and a prefilled run gives a non-zero checksum if values are set to something non-zero.

**4. Serialise the Numba timer.** **Done 2026-10-04.** See Completed.

**5. Narrow the Python/C++ gap.** Write an `@njit` open-addressing hash table over byte keys with FNV-1a, as a counterpart of `dense_hash_map` with `Fnv1a64`, and use it in place of `numba.typed.Dict`.
*Done when:* Python p99 is within about 1.3× of C++ on the same machine, and FINDINGS' "fnv1a kept only for parity" note no longer applies.

**6. Reduce run-to-run noise (Linux).** Document, and optionally add, CPU pinning (`taskset -c N`) and checking for an invariant TSC (the `constant_tsc nonstop_tsc` CPU flags). Print a warning in Settings if the TSC isn't invariant.

### P3: Usability

**7. Command-line flags as well as environment variables.** Add `--batches N` and `--count N`. Keep `LAZYRAM_BATCHES`/`LAZYRAM_COUNT` working, with flags taking precedence, and show the source in Settings (`[--batches]`, `[LAZYRAM_BATCHES]` or `[default]`).

**8. Summary-only and machine-readable output.** A full default run prints 1,000,000 batch lines. Add:
- `--summary-only`, which skips the per-batch lines;
- `--csv FILE`, which writes `batch,p99_ns,max_ns` rows for plotting.

Both go in both languages, and the README gets an example.

**9. More percentiles in the Summary.** Track a fixed-bucket histogram of all samples (constant memory) to report overall p50/p99/p99.9 across the whole run, not just statistics of the batch p99s.

### P4: Reach

**10. Apple Silicon / ARM64 support.** Read `cntvct_el0` on ARM64 (and its frequency from `cntfrq_el0`). On macOS, `mach_absolute_time` is an option. Put this behind the existing zprobe API, so `#error` applies only to truly unsupported CPUs.
*Done when:* it builds and runs on an M-series Mac, and the README requirements are updated.

**11. Precise cache flushing.** Add an optional `clflush`/`clflushopt` path on x86 that evicts just the map's memory, which is far faster than walking a 40 MB array. Select it with `scrub_cache=2` and document it.

## Open questions (ask the user before acting)

- Should item 3's prefill mode become the **default**? It changes what past results mean.
- Is a version number / `CHANGELOG.md` wanted once CI exists?
- Should the old `zdump`/`zdumplite` printers be deleted, now that `main` formats the report itself?

## Completed

- **2026-10-04: Timer check and purpose documentation.**
  - Confirmed C++ uses `CPUID;RDTSC` (start) and `RDTSCP;CPUID` (stop); EAX is now zeroed before CPUID.
  - The Numba timer now emits the same fenced instructions through inline asm (was unfenced RDTSC).
  - README explains the deep-sleep purpose, the sweep procedure, how to read results, and why RDTSCP is used only for the stop read.

- **2026-10-04: Initial code.**
  - C++20 layout (`include/`, `src/`, `third_party/`, `python/`); GCC 15 `register` fix; Makefile.
  - Python 3.12 + Numba port with RDTSC timing.
  - Explicit calibration; samples cleared per batch.
  - Friendly report and argument validation (`--help`).
  - README, CLAUDE.md, FINDINGS.lazy-ram.md (local).
