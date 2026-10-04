/*
 * zprobes.cpp
 *
 *  Created on: Aug 12, 2018
 *      Author: P.R. Adhikary and Santanu C.
 */
#include "lazyram/zprobes.h"

#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace zprobe
{

ZMap   zvalues;
double nanos_per_cycle = 1.0;

namespace
{
uint64_t start_time_sec    = 0;
uint64_t start_time_millis = 0;
uint64_t start_time_micros = 0;
uint64_t start_time_nanos  = 0;
long     start_counter     = 0;
long     self_adjustment   = 0;
} // namespace

void calibrate()
{
  // Average cost of an empty zstart()/zstop() pair; subtracted from every
  // zstop() so samples measure only the code between them.
  constexpr int kRounds = 1000;
  self_adjustment       = 0;
  long sum              = 0;
  for (int i = 0; i < kRounds; ++i)
  {
    long t1 = zstart();
    long t2 = zstop();
    sum += t2 - t1;
  }
  self_adjustment = sum / kRounds;

  timeval tbegin, tend;
  long    begin = zstart();
  gettimeofday(&tbegin, nullptr);
  sleep(2);
  gettimeofday(&tend, nullptr);
  long end = zstop();

  long cycles = end - begin;
  long micros = (tend.tv_sec - tbegin.tv_sec) * 1000 * 1000 + tend.tv_usec - tbegin.tv_usec;
  nanos_per_cycle = micros * 1000.0 / cycles;

  timeval t;
  gettimeofday(&t, nullptr);
  start_counter     = zimmediate();
  start_time_sec    = t.tv_sec;
  start_time_millis = t.tv_sec * 1000ULL + t.tv_usec / 1000;
  start_time_micros = t.tv_sec * 1000000ULL + t.tv_usec;
  start_time_nanos  = t.tv_sec * 1000000000ULL + t.tv_usec * 1000ULL;
}

double mean(const std::vector<long>& data)
{
  double s = 0.0;
  for (long v : data)
    s += v;
  return s / data.size();
}

double std_dev(const std::vector<long>& data)
{
  double m = mean(data), sum_deviation = 0.0;
  for (long v : data)
  {
    double tmp = v - m;
    sum_deviation += tmp * tmp;
  }
  return std::sqrt(sum_deviation / data.size());
}

long zstart() { return static_cast<long>(rdtsc_start()); }

long zstop() { return static_cast<long>(rdtsc_stop()) - self_adjustment; }

long zimmediate() { return static_cast<long>(rdtsc_immediate()); }

long ztimesec()
{
  return start_time_sec + (zimmediate() - start_counter) * nanos_per_cycle / 1000000000;
}

long ztimemillis()
{
  return start_time_millis + (zimmediate() - start_counter) * nanos_per_cycle / 1000000;
}

uint64_t ztimemicros()
{
  return start_time_micros + (zimmediate() - start_counter) * nanos_per_cycle / 1000;
}

uint64_t ztimenanos()
{
  return start_time_nanos + (zimmediate() - start_counter) * nanos_per_cycle;
}

void zlog(const std::string& id, long value) { zvalues[id].push_back(value); }

namespace
{
// Value at percentile p of an already-sorted sample.
long percentile_value(const std::vector<long>& sorted, int p)
{
  long index = static_cast<long>(sorted.size()) * p / 100 - 1;
  return sorted[std::max(index, 0L)];
}
} // namespace

void zdump(const std::string& id)
{
  std::vector<long>& values = zvalues[id];
  if (values.empty())
    return;
  std::sort(values.begin(), values.end());

  std::cout << "Sample size: " << values.size() << '\n';
  for (int p : {50, 95, 99, 100})
  {
    long v = percentile_value(values, p);
    std::cout << id << ":\t" << p << ":\t" << v << ":\t" << v * nanos_per_cycle << '\n';
  }

  double mc     = mean(values);
  double stddev = std_dev(values);
  std::cout << id << ":\tmean:\t" << mc << ":\t" << mc * nanos_per_cycle << '\n';
  std::cout << id << ":\tstddev:\t" << stddev << ":\t" << stddev * nanos_per_cycle << '\n';

  std::cout << id << ": 20 worst\n";
  for (size_t i = values.size() > 20 ? values.size() - 20 : 0; i < values.size(); ++i)
    std::cout << values[i] << ":\t" << values[i] * nanos_per_cycle << '\n';
}

void zdump()
{
  for (const auto& [id, values] : zvalues)
    zdump(id);
}

void zdump_percentiles(const std::string& id, const std::vector<int>& percentiles)
{
  std::vector<long>& values = zvalues[id];
  if (values.empty())
    return;
  std::sort(values.begin(), values.end());

  for (int p : percentiles)
  {
    long value_ns = std::lround(percentile_value(values, p) * nanos_per_cycle);
    if (value_ns > 1000)
      std::cout << "\x1b[31m\x1b[1m" << id << ":\t" << p << ":\t" << value_ns << "\x1b[0m\n";
    else
      std::cout << id << ":\t" << p << ":\t" << value_ns << '\n';
  }
}

void zdumplite(const std::string& id) { zdump_percentiles(id, {99}); }

long zpercentile_ns(const std::string& id, int p)
{
  auto it = zvalues.find(id);
  if (it == zvalues.end() || it->second.empty())
    return 0;
  std::vector<long>& values = it->second;
  std::sort(values.begin(), values.end());
  return std::lround(percentile_value(values, p) * nanos_per_cycle);
}

long probe_overhead_cycles() { return self_adjustment; }

void zreset(const std::string& id) { zvalues.erase(id); }

} // namespace zprobe
