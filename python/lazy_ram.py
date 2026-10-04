#!/usr/bin/env python3
"""
lazy_ram.py

Python implementation of the lazy-ram C++ benchmark (src/main.cpp,
src/zprobes.cpp), which checks whether main memory goes into a power-saving
sleep when idle: it times hash-map lookups after a configurable idle gap. The hot paths are JIT-compiled with Numba so
the timed region runs as machine code, close to the C++ version.

Author      : P.R. Adhikary and Santanu C.
Version     : 1.0

Requires Python 3.12 with numpy and numba (see python/requirements.txt).

Usage:
    python lazy_ram.py <sleep_usec> <scrub_cache>     (python lazy_ram.py --help)
"""

import os
import sys
import time
from collections import namedtuple

import llvmlite.binding as llvm
import numpy as np
from llvmlite import ir
from numba import njit, types
from numba.core import cgutils
from numba.extending import intrinsic
from numba.typed import Dict

# ---------------------------------------------------------------------------
# zprobe -- mirrors include/lazyram/zprobes.h / src/zprobes.cpp.
#
# The timer reads are the same instructions as the C++ probes (Intel's
# benchmarking white-paper pattern), emitted as inline assembly:
#   rdtsc_start(): CPUID; RDTSC   -- serialise, then read the counter
#   rdtsc_stop():  RDTSCP; CPUID  -- read once the lookup has finished,
#                                    then block later instructions
# rdtsc() is an unfenced read (llvm.readcyclecounter) used only by the
# busy-wait loop, never to time the measured region.
# ---------------------------------------------------------------------------
# Numba's JIT doesn't load the native assembly parser that inline asm needs.
llvm.initialize_native_asmparser()

_CLOBBERS = "~{eax},~{ebx},~{ecx},~{edx},~{dirflag},~{fpsr},~{flags},~{memory}"


def _tsc_asm(asm: str):
    """Build an intrinsic that runs `asm` and returns EDX:EAX as one int64."""

    @intrinsic
    def _read(typingctx):
        def codegen(context, builder, signature, args):
            i32, i64 = ir.IntType(32), ir.IntType(64)
            fnty = ir.FunctionType(ir.LiteralStructType([i32, i32]), [])
            pair = builder.call(ir.InlineAsm(fnty, asm, "=r,=r," + _CLOBBERS, side_effect=True), [])
            hi = builder.zext(builder.extract_value(pair, 0), i64)
            lo = builder.zext(builder.extract_value(pair, 1), i64)
            return builder.or_(builder.shl(hi, ir.Constant(i64, 32)), lo)

        return types.int64(), codegen

    return _read


rdtsc_start = _tsc_asm("xorl %eax, %eax\n\tcpuid\n\trdtsc\n\t"
                       "movl %edx, $0\n\tmovl %eax, $1")
rdtsc_stop = _tsc_asm("rdtscp\n\tmovl %edx, $0\n\tmovl %eax, $1\n\t"
                      "xorl %eax, %eax\n\tcpuid")


@intrinsic
def rdtsc(typingctx):
    def codegen(context, builder, signature, args):
        fnty = ir.FunctionType(ir.IntType(64), [])
        fn = cgutils.get_or_insert_function(builder.module, fnty, "llvm.readcyclecounter")
        return builder.call(fn, [])

    return types.int64(), codegen


@njit(cache=True)
def zstart() -> int:
    return rdtsc_start()


@njit(cache=True)
def zstop(self_adjustment: int) -> int:
    return rdtsc_stop() - self_adjustment


@njit(cache=True)
def _probe_overhead(rounds: int) -> int:
    total = 0
    for _ in range(rounds):
        t1 = zstart()
        t2 = zstop(0)
        total += t2 - t1
    return total // rounds


self_adjustment = 0
nanos_per_cycle = 1.0
zvalues: dict[str, np.ndarray] = {}


def calibrate() -> None:
    """Measure probe overhead and the cycle->ns factor (sleeps for 2 s)."""
    global self_adjustment, nanos_per_cycle
    self_adjustment = _probe_overhead(1000)

    c0, t0 = zstart(), time.perf_counter_ns()
    time.sleep(2)
    c1, t1 = zstop(self_adjustment), time.perf_counter_ns()
    nanos_per_cycle = (t1 - t0) / (c1 - c0)


def zlog(id_: str, values: np.ndarray) -> None:
    zvalues[id_] = np.concatenate((zvalues[id_], values)) if id_ in zvalues else values


def zdump_percentiles(id_: str, percentiles) -> None:
    values = np.sort(zvalues.get(id_, np.empty(0, np.int64)))
    n = len(values)
    if n == 0:
        return
    for p in percentiles:
        index = max(n * p // 100 - 1, 0)
        value_ns = round(values[index] * nanos_per_cycle)
        line = f"{id_}:\t{p}:\t{value_ns}"
        if value_ns > 1000:
            print(f"\x1b[31m\x1b[1m{line}\x1b[0m")
        else:
            print(line)


def zdumplite(id_: str) -> None:
    zdump_percentiles(id_, [99])


def zreset(id_: str) -> None:
    zvalues.pop(id_, None)


def zpercentile_ns(id_: str, p: int) -> int:
    """Percentile p (0-100) of the samples logged under `id_`, in ns (0 if none).
    p = 100 gives the slowest sample."""
    values = np.sort(zvalues.get(id_, np.empty(0, np.int64)))
    n = len(values)
    if n == 0:
        return 0
    return round(values[max(n * p // 100 - 1, 0)] * nanos_per_cycle)


# ---------------------------------------------------------------------------
# FNV-1a 64-bit -- the hasher the C++ dense_hash_map uses (Fnv1a64). Numba's
# typed Dict can't take a custom hash, so it isn't wired into `data`; it is
# kept for parity with the C++ source.
# ---------------------------------------------------------------------------
_FNV_PRIME = np.uint64(1099511628211)
_FNV_SEED = np.uint64(14695981039346656037)


@njit(cache=True)
def fnv1a(data: bytes) -> np.uint64:
    h = _FNV_SEED
    for byte in data:
        h = (np.uint64(byte) ^ h) * _FNV_PRIME
    return h


# ---------------------------------------------------------------------------
# Value -- mirrors `struct Value { char foo[20]; int v; };`
# ---------------------------------------------------------------------------
Value = namedtuple("Value", ["foo", "v"])
_FOO_ZERO = (np.uint8(0),) * 20
value_type = types.NamedTuple((types.UniTuple(types.uint8, 20), types.int32), Value)


# ---------------------------------------------------------------------------
# Benchmark -- mirrors main() in src/main.cpp
# ---------------------------------------------------------------------------
@njit(cache=True)
def mysleep(sleep_usec: int, nanos_per_cycle: float) -> None:
    """Busy-wait spin for `sleep_usec` microseconds, so the core never sleeps."""
    target = sleep_usec * 1000.0 / nanos_per_cycle
    t0 = rdtsc()
    while rdtsc() - t0 < target:
        pass


@njit(cache=True)
def blow_cache(data_alt: np.ndarray) -> int:
    """Evict the CPU cache by touching an array far larger than any cache level."""
    for i in range(data_alt.size):
        data_alt[i] = np.random.randint(0, 2**31 - 1)
    total = 0
    for i in range(data_alt.size):
        total += data_alt[i]
    return total


@njit(cache=True)
def run_batch(j, sleep_usec, scrub_cache, data, data_alt, nanos_per_cycle, self_adjustment):
    """One batch of 100 timed lookups; returns (cycle samples, partial sum)."""
    samples = np.empty(100, np.int64)
    total = 0
    for k in range(100):
        if sleep_usec:
            mysleep(sleep_usec, nanos_per_cycle)

        key = str(k + j * 100)

        if scrub_cache:
            total += blow_cache(data_alt)

        t1 = zstart()
        # Like dense_hash_map::operator[], a missing key inserts a default Value.
        if key not in data:
            data[key] = Value(_FOO_ZERO, np.int32(0))
        total += data[key].v
        t2 = zstop(self_adjustment)
        samples[k] = t2 - t1
    return samples, total


LOOKUPS_PER_BATCH = 100
RED_ABOVE_NS = 1000

USAGE = """\
usage: {prog} <sleep_usec> <scrub_cache>

Measures how long a single hash-map lookup (a main-memory access) takes, in
nanoseconds. Run it with increasing sleep_usec: if lookups get slower as the
idle gap grows, memory is going into a power-saving sleep between accesses.

arguments:
  sleep_usec   idle microseconds (busy-wait) before every lookup (0 = no gap)
  scrub_cache  1 = flush the CPU caches before every lookup (cold lookups)
               0 = leave the caches alone (warm lookups)

environment:
  LAZYRAM_BATCHES  number of batches of 100 lookups (default 1000000)
  LAZYRAM_COUNT    ints in the cache-flush array, also the map's
                   initial capacity (default 10000000)

examples:
  LAZYRAM_BATCHES=5 {prog} 0 0
      quick try: set LAZYRAM_BATCHES for this one command so it runs only
      5 batches (500 lookups) instead of the default 1,000,000 batches
  LAZYRAM_BATCHES=5 LAZYRAM_COUNT=200000 {prog} 10 1
      wait 10 us and flush the caches before each lookup; a smaller flush
      array keeps the run short"""


def parse_count(text: str) -> int | None:
    """Non-negative integer, or None for anything else."""
    return int(text) if text.isdigit() else None


def env_or(name: str, fallback: int) -> int:
    value = parse_count(os.environ.get(name, ""))
    return value if value else fallback


def format_ns(ns: int, width: int, color: bool) -> str:
    """'    341 ns', in red when slower than RED_ABOVE_NS on a colour terminal."""
    text = f"{ns:,} ns".rjust(width)
    return f"\x1b[31m\x1b[1m{text}\x1b[0m" if color and ns > RED_ABOVE_NS else text


def setting(label: str, value: str, source: str) -> None:
    print(f"  {label:<24}: {value:<30}{source}")


def main() -> int:
    prog = sys.argv[0]
    args = sys.argv[1:]
    if args in (["-h"], ["--help"]):
        print(USAGE.format(prog=prog))
        return 0

    parsed = [parse_count(a) for a in args]
    if len(args) != 2 or None in parsed or parsed[1] > 1:
        print("error: expected two arguments: <sleep_usec> (0 or more) and <scrub_cache> (0 or 1)\n",
              file=sys.stderr)
        print(USAGE.format(prog=prog))
        return 1

    sleep_usec, scrub_arg = parsed
    scrub_cache = scrub_arg == 1
    count = env_or("LAZYRAM_COUNT", 10_000_000)
    batches = env_or("LAZYRAM_BATCHES", 1_000_000)
    color = sys.stdout.isatty() and "NO_COLOR" not in os.environ
    total_lookups = batches * LOOKUPS_PER_BATCH

    print("lazy-ram: hash-map lookup latency benchmark\n\nSettings")
    setting("Wait before each lookup",
            f"{sleep_usec:,} us (busy-wait)" if sleep_usec else "none",
            f"[arg 1: sleep_usec = {sleep_usec}]")
    setting("Flush CPU cache first", f"yes, {count:,} ints" if scrub_cache else "no",
            f"[arg 2: scrub_cache = {scrub_arg}]")
    setting("Lookups", f"{batches:,} x {LOOKUPS_PER_BATCH} = {total_lookups:,}",
            "[batches from LAZYRAM_BATCHES]" if "LAZYRAM_BATCHES" in os.environ
            else "[default batches; set LAZYRAM_BATCHES]")
    print("\nCalibrating timer (about 2 s)... ", end="", flush=True)

    calibrate()

    # Warm-up: force JIT compilation outside the measured run.
    run_batch(0, 0, False, Dict.empty(key_type=types.unicode_type, value_type=value_type),
              np.zeros(1, np.int32), nanos_per_cycle, self_adjustment)

    print(f"done: 1 CPU cycle = {nanos_per_cycle:.3f} ns, "
          f"timer overhead = {self_adjustment} cycles (subtracted)\n")
    print("Batch    p99 lookup time   (99% of the batch's 100 lookups were this fast or faster)")

    # Numba's typed Dict grows on its own; there is no resize/load-factor
    # tuning like dense_hash_map has.
    data = Dict.empty(key_type=types.unicode_type, value_type=value_type)
    data_alt = np.zeros(count, np.int32)
    batch_p99 = []
    slowest = 0
    total_sum = 0

    for j in range(batches):
        id_ = f"test{j}"
        samples, partial = run_batch(j, sleep_usec, scrub_cache, data, data_alt,
                                     nanos_per_cycle, self_adjustment)
        total_sum += partial
        zlog(id_, samples)
        batch_p99.append(zpercentile_ns(id_, 99))
        slowest = max(slowest, zpercentile_ns(id_, 100))
        zreset(id_)
        print(f"{j:>5}   {format_ns(batch_p99[-1], 15, color)}")

    batch_p99.sort()
    print(f"\nSummary ({total_lookups:,} lookups)")
    print(f"  Typical batch p99 (median) : {format_ns(batch_p99[batches // 2], 12, color)}")
    print(f"  Worst batch p99            : {format_ns(batch_p99[-1], 12, color)}")
    print(f"  Slowest single lookup      : {format_ns(slowest, 12, color)}")
    print(f"\nChecksum: {total_sum} (printed only so the compiler cannot optimise the lookups away)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
