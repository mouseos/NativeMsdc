// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
namespace NativeOverlapTrace {
struct Event { uint64_t ns, tid, block, count; uint32_t pid, window; };
struct Snapshot { Event events[32]; uint64_t count, full, contention; };
class Buffer {
  unsigned lock = 0;
  Snapshot data = {};
  uint64_t contention = 0;
public:
  bool record(const Event &event) {
    if (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_add(&contention, 1ULL, __ATOMIC_RELAXED); return false;
    }
    bool stored = data.count < 32;
    if (stored) data.events[data.count++] = event; else ++data.full;
    __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE);
    return stored;
  }
  bool drain(Snapshot &out) {
    if (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) return false;
    out = data; data = {};
    __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE);
    out.contention = __atomic_exchange_n(&contention, 0ULL, __ATOMIC_RELAXED);
    return true;
  }
};
}
