// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
namespace NativeOverlapTrace {
struct Event { uint64_t ns, tid, block, count; uint32_t pid, window; };
struct Snapshot { Event events[32]; uint64_t count, full, contention; };
class Buffer {
  unsigned lock = 0, saturated = 0;
  Snapshot data = {};
  uint64_t full = 0, contention = 0;
public:
  // Build identity and timestamp only after a slot has been secured. Once
  // full, producers need only an atomic load and loss-counter increment.
  template <typename MakeEvent> bool record(MakeEvent makeEvent) {
    if (__atomic_load_n(&saturated, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_add(&full, 1ULL, __ATOMIC_RELAXED); return false;
    }
    if (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_add(&contention, 1ULL, __ATOMIC_RELAXED); return false;
    }
    if (data.count == 32) {
      __atomic_store_n(&saturated, 1u, __ATOMIC_RELEASE);
      __atomic_fetch_add(&full, 1ULL, __ATOMIC_RELAXED);
      __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE); return false;
    }
    data.events[data.count++] = makeEvent();
    if (data.count == 32) __atomic_store_n(&saturated, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE);
    return true;
  }
  bool drain(Snapshot &out) {
    if (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) return false;
    out = data; data = {};
    __atomic_store_n(&saturated, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE);
    // Counters are exact across quiescent drains but sampled independently:
    // a producer racing a drain can be accounted in an adjacent interval.
    out.full = __atomic_exchange_n(&full, 0ULL, __ATOMIC_RELAXED);
    out.contention = __atomic_exchange_n(&contention, 0ULL, __ATOMIC_RELAXED);
    return true;
  }
};
}
