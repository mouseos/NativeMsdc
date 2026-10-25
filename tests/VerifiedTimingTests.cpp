// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../NativeMsdc.h"
using namespace NativeMsdcHw;
int main ()
{
  volatile uint64_t stats[6] = {};
  unsigned clocks = 0, calls = 0;
  uint64_t clock = 100;
  auto now = [&] () { ++clocks; uint64_t n = clock; clock += 25; return n; };
  auto read = [&] () { ++calls; return 7; };
  assert (verifiedTimedOperation (false, stats, 0, 4096, now, read) == 7);
  assert (!clocks && calls == 1 && !stats[0] && !stats[1] && !stats[2]);
  assert (verifiedTimedOperation (true, stats, 0, 4096, now, read) == 7);
  assert (clocks == 2 && calls == 2 && stats[0] == 1 && stats[1] == 4096 && stats[2] == 25);
  auto verify = [&] () { ++calls; return 0; };
  assert (!verifiedTimedOperation (true, stats, 3, 1536, now, verify));
  assert (stats[3] == 1 && stats[4] == 1536 && stats[5] == 25 && stats[0] == 1);
  assert (!verifiedTimedOperation (true, stats, 3, 4096, now, verify));
  assert (stats[3] == 2 && stats[4] == 5632 && stats[5] == 50);
  // Unsigned elapsed subtraction remains well defined across a counter wrap.
  clock = UINT64_MAX - 10;
  assert (!verifiedTimedOperation (true, stats, 3, 512, now, verify));
  assert (stats[5] == 75);
  puts ("NativeMsdc actual timing helper passed: disabled no clock, original status, read/verify separation, cumulative metrics, wrap");
}
