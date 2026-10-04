# lazy-ram

lazy-ram checks whether **main memory falls into a power-saving ("deep sleep") state when it sits idle**, and how much that slows the next access.

It does this by timing hash-map lookups, each one a trip to main memory, while you increase the idle gap between them (the first argument, `sleep_usec`). Memory should stay awake and answer just as fast after a long gap as after a short one. **If lookup times climb as the gap grows, the memory, or the memory controller, is going to sleep between accesses and paying a wake-up cost.** That cost shows up directly in the output.

Each lookup is timed in CPU cycles, and the report focuses on the slow end (99th percentile and the single slowest lookup), because wake-up delays show up as occasional slow accesses rather than a shift in the average. See [Detecting memory sleep](#detecting-memory-sleep) for how to run the test and read the result.

**Workflow diagram:** [`docs/workflow.png`](docs/workflow.png) shows how the code works in three pictures: one run step by step (including the timed region), which parts talk to what, and the memory deep-sleep effect the benchmark detects. It also has a table mapping the C++ to the Python version. Click the link to open it in GitHub's image viewer.

There are two implementations, and they produce the same output:

| | Source | How it times |
|---|---|---|
| C++ | `src/main.cpp`, `src/zprobes.cpp` | CPU cycle counter: `CPUID`+`RDTSC` to start, `RDTSCP`+`CPUID` to stop (inline assembly) |
| Python | `python/lazy_ram.py` | the same instructions, emitted as inline assembly from Numba-compiled code |

## Quick start

```sh
make                                  # build the C++ program -> build/lazy_ram
LAZYRAM_BATCHES=5 ./build/lazy_ram 0 0
```

This runs a short test: 5 batches of 100 lookups (500 in total), with no pause and no cache flushing. It finishes in about 3 seconds, most of which is the start-up calibration. The rest of this README explains each part of that command and its output.

## The command line

```
./build/lazy_ram <sleep_usec> <scrub_cache>
```

Both arguments are required, and they always come in this order. Run `./build/lazy_ram --help` to see this summary in the terminal.

| Position | Name | Allowed values | What it does |
|---|---|---|---|
| 1st | `sleep_usec` | `0` or any whole number | Microseconds of **idle time before every lookup**. `0` means no gap. **This is the knob for the deep-sleep test:** run it at increasing values and watch whether lookups get slower. The program spins in a busy loop rather than sleeping, so the CPU core itself stays awake and any slowdown comes from the memory side, not from the core waking up. |
| 2nd | `scrub_cache` | `0` or `1` | `1` = **flush the CPU caches before every lookup**, by writing to and reading a large array (see `LAZYRAM_COUNT`), so each lookup has to fetch from main memory ("cold"). `0` = leave the caches alone, so lookups usually hit the cache ("warm"). |

Examples:

| Command | Meaning |
|---|---|
| `./build/lazy_ram 0 0` | back-to-back lookups, warm caches: the best case |
| `./build/lazy_ram 100 0` | wait 100 µs before each lookup, caches untouched |
| `./build/lazy_ram 0 1` | flush the caches before each lookup: the cold, worst case |
| `./build/lazy_ram 50 1` | wait 50 µs, then flush the caches, before each lookup |

## Environment variables (optional)

Two settings come from environment variables instead of arguments. Put them **in front of the command**:

```sh
LAZYRAM_BATCHES=5 ./build/lazy_ram 0 0
```

This is ordinary shell syntax. `LAZYRAM_BATCHES=5` applies **to this one command only**: it doesn't change your shell, and the next command goes back to the default. In this example it means "run just 5 batches (5 × 100 = 500 lookups) instead of the default 1,000,000 batches (100 million lookups)", which turns a run of several minutes into a few seconds. To keep a setting for the rest of your terminal session, use `export LAZYRAM_BATCHES=5`.

| Variable | Meaning | Default |
|---|---|---|
| `LAZYRAM_BATCHES` | how many **batches** to run. Every batch has 100 lookups and prints one result line. | `1000000` |
| `LAZYRAM_COUNT` | how many integers the cache-flush array holds (4 bytes each), and the hash map's starting capacity. The default of 10,000,000 (40 MB) is bigger than any CPU cache. Lower it to make `scrub_cache=1` runs faster. | `10000000` |

With `scrub_cache=1`, every lookup first walks the whole flush array, so keep both settings small for a first try:

```sh
LAZYRAM_BATCHES=5 LAZYRAM_COUNT=200000 ./build/lazy_ram 10 1
```

## Reading the output

```
lazy-ram: hash-map lookup latency benchmark

Settings
  Wait before each lookup : none                          [arg 1: sleep_usec = 0]
  Flush CPU cache first   : no                            [arg 2: scrub_cache = 0]
  Lookups                 : 5 x 100 = 500                 [batches from LAZYRAM_BATCHES]

Calibrating timer (about 2 s)... done: 1 CPU cycle = 0.417 ns, timer overhead = 22 cycles (subtracted)

Batch    p99 lookup time   (99% of the batch's 100 lookups were this fast or faster)
    0            341 ns
    1            428 ns
    2            358 ns
    3            368 ns
    4            367 ns

Summary (500 lookups)
  Typical batch p99 (median) :       367 ns
  Worst batch p99            :       428 ns
  Slowest single lookup      :     1,912 ns

Checksum: 0 (printed only so the compiler cannot optimise the lookups away)
```

| Section | What it tells you |
|---|---|
| **Settings** | What this run is doing, and where each setting came from (`[arg 1]`, `[arg 2]`, or the environment variable). Check this first to confirm the arguments were read the way you meant. |
| **Calibrating timer** | The program pauses for about 2 seconds to work out how long one CPU cycle lasts (`0.417 ns` = a 2.4 GHz clock). It also measures how many cycles the timer itself costs; that overhead is subtracted from every measurement (see [How the timer works](#how-the-timer-works-rdtsc-and-rdtscp)). |
| **Batch table** | One line per batch. **p99** (99th percentile) is the time that 99 of the batch's 100 lookups beat, so it shows how slow lookups get, not just the average. On a colour terminal, values above 1,000 ns are shown in red. |
| **Summary** | All batches together. *Typical batch p99* is the middle (median) of the batch results. *Worst batch p99* is the largest of them. *Slowest single lookup* is the slowest individual lookup in the whole run, often caused by the operating system interrupting the program. |
| **Checksum** | Not a result. The program adds up the looked-up values and prints the total so the compiler can't skip the lookups as unused work. It is `0` with `scrub_cache=0`, because every map value is zero. With `scrub_cache=1` it also includes the flush array's sum, so it's a large number. |

## Detecting memory sleep

This is what lazy-ram is for. Keep `scrub_cache` at `0` and run the same command at increasing idle gaps:

```sh
for gap in 0 10 100 1000 10000; do
  echo "== sleep_usec=$gap"
  LAZYRAM_BATCHES=20 ./build/lazy_ram $gap 0 | grep -E "Typical|Slowest"
done
```

The 10,000 µs run takes about 20 s, because 20 batches × 100 lookups × 10 ms of waiting adds up. Use more batches for steadier numbers.

**How to read the result**

| What you see as `sleep_usec` grows | What it means |
|---|---|
| *Typical batch p99* and *Slowest single lookup* stay about the same | ✅ Memory stays awake. Idle time doesn't make the next access slower. This is the expected, healthy result. |
| They rise once the gap passes some threshold, e.g. flat up to 100 µs, then higher from 1,000 µs | ⚠️ Something in the memory path is entering a power-saving state after that much idle time, and the first access afterwards pays to wake it. The gap where the jump starts is roughly how long it takes to fall asleep. The size of the jump is the wake-up cost. |
| Only *Slowest single lookup* jumps, occasionally | Usually the operating system interrupting the program. Rerun with more batches; if it keeps getting worse with longer gaps, treat it as a sleep effect. |

Example from an Intel MacBook Pro (5 batches per run, so indicative rather than precise):

| `sleep_usec` | Typical batch p99 | Slowest single lookup |
|---|---|---|
| 0 | 422 ns | 622 ns |
| 10 | 441 ns | 752 ns |
| 100 | 420 ns | 616 ns |
| 1,000 | 530 ns | 20,266 ns |
| 10,000 | 580 ns | 22,619 ns |

Here memory stayed responsive up to 100 µs gaps. From 1 ms gaps the typical p99 rose about 25–40%, and the slowest lookup jumped from about 0.6 µs to about 20 µs: a sign that the memory system is powering down during millisecond-long idle periods.

**Why the lookups really reach main memory.** The hash map is pre-sized to 10 million slots (over 1 GB in RAM), far larger than any CPU cache, and every lookup uses a new key that lands in a random slot. So each timed lookup is a genuine main-memory access, and a sleeping memory system can't hide behind the cache.

**What can be "going to sleep".** Typical culprits are DRAM power-down or self-refresh modes, and the memory controller or "uncore" lowering its clock when there's no traffic. Because the program busy-waits, the CPU core stays awake throughout, so a slowdown points at the memory side rather than at core sleep states. Fixing it is a platform matter: BIOS/firmware memory power settings, OS power profile, or C-state and uncore-frequency settings on Linux servers.

**Other comparisons.** `scrub_cache=1` makes the program flush the CPU caches before every lookup, which gives a "worst case, always cold" reference to compare against. Timings vary between runs and machines, so always compare runs made on the same machine, close together in time.

## Requirements

- An **x86_64** CPU: Linux, or macOS on an Intel Mac. Apple Silicon isn't supported, because the timer needs the x86 `RDTSC` instruction.
- C++: **GCC 15+** on Linux or **Apple clang** (Xcode Command Line Tools: `xcode-select --install`) on macOS; any C++20 compiler works. You also need `make`.
- Python version: **Python 3.12**.
  - macOS: `brew install python@3.12` or `uv python install 3.12`
  - Linux: your distro's `python3.12` package (plus `python3.12-venv` on Debian/Ubuntu), or `uv python install 3.12`

## Building (C++)

```sh
make                      # optimised build: build/lazy_ram (-O3)
make debug                # debugging build: build/debug/lazy_ram (-O0 -g)
make run ARGS="0 1"       # build, then run with the given arguments
make clean                # delete build/
make CXX=clang++          # use a different compiler
make CXXFLAGS="-std=c++20 -O3 -march=native"   # extra tuning
```

## Running the Python version

```sh
make venv                             # one-time: creates .venv and installs numpy + numba
LAZYRAM_BATCHES=5 make run-python ARGS="0 0"
```

If `python3.12` isn't on your `PATH`, tell `make` where it is: `make venv PYTHON=/path/to/python3.12` (with uv: `make venv PYTHON="$(uv python find 3.12)"`).

To set it up by hand instead:

```sh
python3.12 -m venv .venv
.venv/bin/pip install -r python/requirements.txt
LAZYRAM_BATCHES=5 .venv/bin/python python/lazy_ram.py 0 0
```

The Python version takes the same arguments, reads the same environment variables and prints the same report. Numba compiles its timed loop to machine code. The first run takes a few extra seconds while that happens, and the result is cached in `python/__pycache__/`. Compilation always finishes before measuring starts, so it never affects the numbers. Expect Python's lookup times to be somewhat higher than the C++ ones, mostly because Numba's dictionary hashes strings differently. On Intel Macs, `requirements.txt` pins `numba<0.63`, because that's the last release with prebuilt Intel-mac packages.

## How it works

1. **Calibrate:** measure the timer's own cost and the length of one CPU cycle (about 2 s).
2. For each batch, do 100 lookups, using keys `"0"`, `"1"`, `"2"`, … in batch 0, then `"100"` … `"199"` in batch 1, and so on. Before each lookup:
   - busy-wait `sleep_usec` microseconds, if it isn't `0`;
   - flush the caches, if `scrub_cache` is `1`.

   Only the lookup itself is timed.
3. The map is a `google::dense_hash_map` with an FNV-1a string hash. It starts empty, so each lookup adds its key with a zero value, the way `map[key]` does in C++.
4. After each batch, print its p99 and clear its samples. At the end, print the summary.

## How the timer works (RDTSC and RDTSCP)

Modern CPUs run instructions out of order, so a naive timer read can happen before the lookup has finished, or after part of the next instruction has already started. lazy-ram brackets each lookup with the pattern Intel recommends in its white paper *How to Benchmark Code Execution Times on Intel IA-32 and IA-64 Instruction Set Architectures*:

```
start:  CPUID ; RDTSC      <- CPUID waits for everything before it, then read the counter
        ... the lookup ...
stop:   RDTSCP ; CPUID     <- RDTSCP reads only after the lookup has finished;
                              CPUID stops later instructions from slipping in before it
```

- **The stop read uses `RDTSCP`.** It's the better instruction for the end of a measurement, because it waits until every earlier instruction, the lookup, has completed.
- **The start read deliberately uses `CPUID` + `RDTSC`, not `RDTSCP`.** `RDTSCP` waits for *earlier* instructions but doesn't stop *later* ones, the lookup, from starting before the counter is read, which would cut part of the lookup out of the measurement. `CPUID` is a full barrier, so the lookup can't begin early.
- The time a start/stop pair takes with nothing between them is measured at calibration ("timer overhead") and subtracted from every sample.

The C++ and Python versions emit the same instructions. `RDTSC` without a barrier is used only for the busy-wait loop, which doesn't need precision.

## Layout

```
include/lazyram/zprobes.h   timing API (zprobe namespace)
src/zprobes.cpp             timer implementation and calibration
src/main.cpp                the benchmark: arguments, loop, report
python/lazy_ram.py          Python/Numba version
python/requirements.txt
docs/workflow.png           workflow diagrams
third_party/sparsehash/     vendored Google sparsehash (header-only)
```

`third_party/sparsehash` is an unmodified copy of [Google sparsehash](https://github.com/sparsehash/sparsehash), distributed under its own BSD-3-Clause license.
