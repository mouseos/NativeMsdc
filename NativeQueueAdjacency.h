// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
namespace NativeQueueAdjacency {
// All methods are called with the existing block queue mutex held.
struct Request {
  uint64_t id, arrival, start, predecessor, gap;
  uint64_t block, count;
  uint32_t window, depth;
  bool valid, adjacent, mergeable, clockBad;
};
struct Event { Request request; uint64_t completedBytes; int32_t status; };
struct Snapshot {
  uint64_t admitted, invalid, tailPairs, adjacent, mergeable, gap1ms, gap5ms, gap10ms;
  uint64_t started, waitNs, waitMaxNs, waits[4], clockBad;
  uint64_t success, authFailure, aborted, otherFailure, partial, eventCount, eventDropped;
  Event events[16];
};
class Collector {
  uint64_t nextId = 0;
  Snapshot stats = {};
public:
  Request admit(uint32_t window, uint64_t block, uint64_t count, uint64_t now,
                unsigned depth, bool valid, const Request *tail) {
    Request r{}; r.id = ++nextId; r.arrival = now; r.window = window;
    r.block = block; r.count = count; r.depth = depth; r.valid = valid;
    ++stats.admitted;
    if (!valid) { ++stats.invalid; return r; }
    if (tail && tail->valid) {
      ++stats.tailPairs; r.predecessor = tail->id;
      if (now < tail->arrival) { r.clockBad = true; ++stats.clockBad; }
      else r.gap = now - tail->arrival;
      if (tail->window == window && block <= UINT64_MAX - count && tail->block <= UINT64_MAX - tail->count &&
          (tail->block + tail->count == block || block + count == tail->block)) {
        r.adjacent = true; ++stats.adjacent;
        if (count <= 128 && tail->count <= 128 - count) { r.mergeable = true; ++stats.mergeable; }
        if (!r.clockBad) { if (r.gap <= 1000000) ++stats.gap1ms; if (r.gap <= 5000000) ++stats.gap5ms; if (r.gap <= 10000000) ++stats.gap10ms; }
      }
    }
    return r;
  }
  void start(Request &r, uint64_t now) {
    r.start = now; ++stats.started;
    if (now < r.arrival) { r.clockBad = true; ++stats.clockBad; return; }
    uint64_t wait = now - r.arrival; stats.waitNs += wait;
    if (wait > stats.waitMaxNs) stats.waitMaxNs = wait;
    ++stats.waits[wait <= 1000000 ? 0 : wait <= 5000000 ? 1 : wait <= 10000000 ? 2 : 3];
  }
  void finish(const Request &r, int32_t status, uint64_t bytes, int32_t success, int32_t authFailure, int32_t aborted) {
    if (status == success) ++stats.success;
    else if (status == authFailure) ++stats.authFailure;
    else if (status == aborted) ++stats.aborted;
    else ++stats.otherFailure;
    if (r.count <= UINT64_MAX / 512 && bytes != r.count * 512) ++stats.partial;
    if (stats.eventCount < 16) stats.events[stats.eventCount++] = {r, bytes, status};
    else ++stats.eventDropped;
  }
  void drain(Snapshot &out) { out = stats; stats = {}; }
};
}
