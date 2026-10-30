// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
namespace NativeQueueWaitCause {
// All operations occur under the existing block queue mutex. No addresses,
// identities, allocations, or per-request event logs are retained.
struct Previous { uint64_t sizeSectors, start, end; bool valid; };
struct Snapshot {
  uint64_t longWait, unknownPrevious, badInput, badPrevious, overflow;
  uint64_t relation[3]; // arrived before, during, or after previous execution
  uint64_t cells[3][4][3][4]; // previous size, duration, arrival relation, depth
};
inline void incrementSaturating(uint64_t &value, uint64_t &overflow) {
  if (value == UINT64_MAX) overflow = 1; else ++value;
}
class Collector {
  Previous previous = {};
  Snapshot stats = {};
  void inc(uint64_t &x) { incrementSaturating(x, stats.overflow); }
public:
  // Called only for non-cancelled jobs. The immediately preceding worker job
  // is a correlation, not a proof that it caused this request's wait.
  void dequeue(uint64_t arrival, uint64_t now, unsigned depth) {
    if (now < arrival || !depth) { inc(stats.badInput); return; }
    if (now - arrival <= 10000000ULL) return;
    inc(stats.longWait);
    if (!previous.valid) { inc(stats.unknownPrevious); return; }
    if (previous.start > previous.end || previous.end > now) { inc(stats.badPrevious); return; }
    unsigned rel = arrival < previous.start ? 0 : arrival <= previous.end ? 1 : 2;
    inc(stats.relation[rel]);
    unsigned size = previous.sizeSectors <= 8 ? 0 : previous.sizeSectors <= 128 ? 1 : 2;
    uint64_t duration = previous.end - previous.start;
    unsigned time = duration <= 1000000ULL ? 0 : duration <= 5000000ULL ? 1 : duration <= 10000000ULL ? 2 : 3;
    unsigned q = depth == 1 ? 0 : depth == 2 ? 1 : depth <= 8 ? 2 : 3;
    inc(stats.cells[size][time][rel][q]);
  }
  void finished(uint64_t sectors, uint64_t begin, uint64_t end, bool valid) {
    previous = {sectors, begin, end, valid && sectors && begin <= end};
  }
  void drain(Snapshot &out) { out = stats; stats = {}; } // preserve predecessor across reports
};
}
