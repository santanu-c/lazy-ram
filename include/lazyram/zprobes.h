/*
 * zprobes.h
 *
 *  Created on: Aug 12, 2018
 *      Author: P.R. Adhikary and Santanu C.
 *
 * Cycle-accurate timing probes built on the x86 time-stamp counter.
 * Call calibrate() once before using any of the other functions.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if !defined(__x86_64__)
#  error "zprobes requires an x86_64 CPU (RDTSC/RDTSCP)"
#endif

namespace zprobe
{

using ZMap = std::map<std::string, std::vector<long>>;

extern ZMap   zvalues;
extern double nanos_per_cycle;

// Timer reads follow Intel's "How to Benchmark Code Execution Times on Intel
// IA-32 and IA-64 Instruction Set Architectures" white paper:
//
//   start: CPUID; RDTSC   -- CPUID is a full serialising barrier, so nothing
//                            before it can still be executing when the
//                            counter is read.
//   stop : RDTSCP; CPUID  -- RDTSCP reads the counter only after every
//                            earlier instruction (the measured lookup) has
//                            finished; the trailing CPUID stops later
//                            instructions from starting before the read.
//
// RDTSCP is not used on the start side: it waits for *earlier* instructions
// but lets *later* ones (the lookup) begin before the read, which would
// shorten the measurement. EAX is zeroed so CPUID always runs the same leaf
// and costs the same every time.
inline uint64_t rdtsc_start()
{
  uint32_t hi, lo;
  asm volatile("xor %%eax, %%eax\n\t"
               "cpuid\n\t"
               "rdtsc\n\t"
               "mov %%edx, %0\n\t"
               "mov %%eax, %1\n\t"
               : "=r"(hi), "=r"(lo)
               :: "%rax", "%rbx", "%rcx", "%rdx");
  return (uint64_t(hi) << 32) | lo;
}

inline uint64_t rdtsc_stop()
{
  uint32_t hi, lo;
  asm volatile("rdtscp\n\t"
               "mov %%edx, %0\n\t"
               "mov %%eax, %1\n\t"
               "xor %%eax, %%eax\n\t"
               "cpuid\n\t"
               : "=r"(hi), "=r"(lo)
               :: "%rax", "%rbx", "%rcx", "%rdx");
  return (uint64_t(hi) << 32) | lo;
}

// Unfenced read, for wall-clock helpers and spin loops only -- never for
// timing a measured region.
inline uint64_t rdtsc_immediate()
{
  uint32_t hi, lo;
  asm volatile("rdtsc\n\t"
               "mov %%edx, %0\n\t"
               "mov %%eax, %1\n\t"
               : "=r"(hi), "=r"(lo)
               :: "%rax", "%rdx");
  return (uint64_t(hi) << 32) | lo;
}

// Measures probe overhead and the cycle->ns factor (sleeps for 2 s).
void calibrate();

long zstart();
long zstop();
long zimmediate();

void zlog(const std::string& id, long value);
void zdump(const std::string& id);
void zdump();
void zdump_percentiles(const std::string& id, const std::vector<int>& percentiles);
void zdumplite(const std::string& id);
void zreset(const std::string& id);

// Percentile p (0-100) of the samples logged under `id`, in nanoseconds
// (0 if there are none). p = 100 gives the slowest sample.
long zpercentile_ns(const std::string& id, int p);

// Probe overhead subtracted from every zstop(), measured by calibrate().
long probe_overhead_cycles();

long     ztimesec();
long     ztimemillis();
uint64_t ztimemicros();
uint64_t ztimenanos();

double mean(const std::vector<long>& data);
double std_dev(const std::vector<long>& data);

// Logs the lifetime of the enclosing scope under `id`.
class ScopedProbe
{
public:
  explicit ScopedProbe(std::string id) : id_(std::move(id)), start_(zstart()) {}
  ~ScopedProbe() { zlog(id_, zstop() - start_); }

  ScopedProbe(const ScopedProbe&)            = delete;
  ScopedProbe& operator=(const ScopedProbe&) = delete;

private:
  std::string id_;
  long        start_;
};

} // namespace zprobe
