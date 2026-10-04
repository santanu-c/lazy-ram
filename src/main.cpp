//============================================================================
// Name        : main.cpp
// Author      : P.R. Adhikary and Santanu C.
// Version     : 1.0
// Description : Hash-map lookup latency micro-benchmark with optional
//               busy-wait and CPU-cache scrubbing between lookups.
//
// Usage       : lazy_ram <sleep_usec> <scrub_cache>   (lazy_ram --help)
//============================================================================

#include "lazyram/zprobes.h"

#include <sparsehash/dense_hash_map>

#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace
{

constexpr long kLookupsPerBatch = 100;
constexpr long kRedAboveNs      = 1000;

// FNV-1a 64-bit hash for std::string keys.
struct Fnv1a64
{
  static constexpr uint64_t kPrime = 1099511628211u;
  static constexpr uint64_t kSeed  = 14695981039346656037u;

  static uint64_t hash(const void* data, size_t num_bytes, uint64_t h = kSeed)
  {
    assert(data);
    auto ptr = static_cast<const unsigned char*>(data);
    while (num_bytes--)
      h = (*ptr++ ^ h) * kPrime;
    return h;
  }

  size_t operator()(const std::string& key) const { return hash(key.data(), key.size()); }
};

struct Value
{
  char foo[20];
  int  v;
};

void print_usage(const char* prog)
{
  std::cout
      << "usage: " << prog << " <sleep_usec> <scrub_cache>\n"
      << "\n"
      << "Measures how long a single hash-map lookup (a main-memory access) takes, in\n"
      << "nanoseconds. Run it with increasing sleep_usec: if lookups get slower as the\n"
      << "idle gap grows, memory is going into a power-saving sleep between accesses.\n"
      << "\n"
      << "arguments:\n"
      << "  sleep_usec   idle microseconds (busy-wait) before every lookup (0 = no gap)\n"
      << "  scrub_cache  1 = flush the CPU caches before every lookup (cold lookups)\n"
      << "               0 = leave the caches alone (warm lookups)\n"
      << "\n"
      << "environment:\n"
      << "  LAZYRAM_BATCHES  number of batches of 100 lookups (default 1000000)\n"
      << "  LAZYRAM_COUNT    ints in the cache-flush array, also the map's\n"
      << "                   initial capacity (default 10000000)\n"
      << "\n"
      << "examples:\n"
      << "  LAZYRAM_BATCHES=5 " << prog << " 0 0\n"
      << "      quick try: set LAZYRAM_BATCHES for this one command so it runs only\n"
      << "      5 batches (500 lookups) instead of the default 1,000,000 batches\n"
      << "  LAZYRAM_BATCHES=5 LAZYRAM_COUNT=200000 " << prog << " 10 1\n"
      << "      wait 10 us and flush the caches before each lookup; a smaller flush\n"
      << "      array keeps the run short\n";
}

// Parses a non-negative integer; returns false on anything else.
bool parse_count(const char* s, long& out)
{
  char* end = nullptr;
  out       = std::strtol(s, &end, 10);
  return end != s && *end == '\0' && out >= 0;
}

long env_or(const char* name, long fallback)
{
  const char* s = std::getenv(name);
  long        v = 0;
  return s && parse_count(s, v) && v > 0 ? v : fallback;
}

// 10000000 -> "10,000,000"
std::string with_commas(long n)
{
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
    s.insert(i, ",");
  return s;
}

bool use_color() { return isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR"); }

// "    341 ns", in red when slower than kRedAboveNs on a colour terminal.
std::string format_ns(long ns, int width, bool color)
{
  std::string text = with_commas(ns) + " ns";
  if (static_cast<int>(text.size()) < width)
    text.insert(0, width - text.size(), ' ');
  return color && ns > kRedAboveNs ? "\x1b[31m\x1b[1m" + text + "\x1b[0m" : text;
}

// Busy-wait spin, so the core never sleeps between lookups.
void mysleep(long sleep_usec)
{
  auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(sleep_usec);
  while (std::chrono::steady_clock::now() < deadline)
  {
  }
}

// Evict the CPU cache by touching an array far larger than any cache level.
long blow_cache(std::vector<int>& data_alt, std::mt19937& rng)
{
  std::uniform_int_distribution<int> dist(0, INT32_MAX);
  for (int& x : data_alt)
    x = dist(rng);
  long sum = 0;
  for (int x : data_alt)
    sum += x;
  return sum;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help"))
  {
    print_usage(argv[0]);
    return 0;
  }

  long sleep_usec = 0, scrub_arg = 0;
  if (argc != 3 || !parse_count(argv[1], sleep_usec) || !parse_count(argv[2], scrub_arg) ||
      scrub_arg > 1)
  {
    std::cerr << "error: expected two arguments: <sleep_usec> (0 or more) and <scrub_cache> (0 or 1)\n\n";
    print_usage(argv[0]);
    return 1;
  }

  const bool scrub_cache = scrub_arg == 1;
  const long count       = env_or("LAZYRAM_COUNT", 10'000'000);
  const long batches     = env_or("LAZYRAM_BATCHES", 1'000'000);
  const bool color       = use_color();

  // "  label : value      [where it came from]"
  auto setting = [](const std::string& label, const std::string& value, const std::string& source) {
    std::cout << "  " << std::left << std::setw(24) << label << ": " << std::setw(30) << value
              << source << std::right << '\n';
  };
  const bool batches_from_env = std::getenv("LAZYRAM_BATCHES") != nullptr;

  std::cout << "lazy-ram: hash-map lookup latency benchmark\n\nSettings\n";
  setting("Wait before each lookup",
          sleep_usec ? with_commas(sleep_usec) + " us (busy-wait)" : "none",
          "[arg 1: sleep_usec = " + std::to_string(sleep_usec) + "]");
  setting("Flush CPU cache first", scrub_cache ? "yes, " + with_commas(count) + " ints" : "no",
          "[arg 2: scrub_cache = " + std::to_string(scrub_arg) + "]");
  setting("Lookups",
          with_commas(batches) + " x " + std::to_string(kLookupsPerBatch) + " = " +
              with_commas(batches * kLookupsPerBatch),
          batches_from_env ? "[batches from LAZYRAM_BATCHES]" : "[default batches; set LAZYRAM_BATCHES]");
  std::cout << '\n'
            << "Calibrating timer (about 2 s)... " << std::flush;

  zprobe::calibrate();

  std::cout << "done: 1 CPU cycle = " << std::fixed << std::setprecision(3)
            << zprobe::nanos_per_cycle << " ns, timer overhead = "
            << zprobe::probe_overhead_cycles() << " cycles (subtracted)\n\n"
            << "Batch    p99 lookup time   (99% of the batch's 100 lookups were this fast or faster)\n";

  google::dense_hash_map<std::string, Value, Fnv1a64> data;
  data.set_empty_key("");
  data.resize(count);
  data.min_load_factor(0.0);
  data.max_load_factor(1.0);

  std::vector<int>  data_alt(count);
  std::mt19937      rng(std::random_device{}());
  std::vector<long> batch_p99(batches);
  long              slowest   = 0;
  long              total_sum = 0;

  for (long j = 0; j < batches; ++j)
  {
    const std::string id = "test" + std::to_string(j);
    for (long k = 0; k < kLookupsPerBatch; ++k)
    {
      if (sleep_usec)
        mysleep(sleep_usec);

      std::string key = std::to_string(k + j * kLookupsPerBatch);

      if (scrub_cache)
        total_sum += blow_cache(data_alt, rng);

      long t1 = zprobe::zstart();
      total_sum += data[key].v;
      long t2 = zprobe::zstop();
      zprobe::zlog(id, t2 - t1);
    }
    batch_p99[j] = zprobe::zpercentile_ns(id, 99);
    slowest      = std::max(slowest, zprobe::zpercentile_ns(id, 100));
    zprobe::zreset(id);

    std::cout << std::setw(5) << j << "   " << format_ns(batch_p99[j], 15, color) << '\n';
  }

  std::sort(batch_p99.begin(), batch_p99.end());
  std::cout << "\nSummary (" << with_commas(batches * kLookupsPerBatch) << " lookups)\n"
            << "  Typical batch p99 (median) : " << format_ns(batch_p99[batches / 2], 12, color) << '\n'
            << "  Worst batch p99            : " << format_ns(batch_p99.back(), 12, color) << '\n'
            << "  Slowest single lookup      : " << format_ns(slowest, 12, color) << '\n'
            << "\nChecksum: " << total_sum
            << " (printed only so the compiler cannot optimise the lookups away)\n";
  return 0;
}
