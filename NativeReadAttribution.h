// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
namespace NativeReadAttribution {
struct Key { uint32_t pid, window, flags, size; uint64_t tid; };
struct Row { bool used; Key key; uint64_t calls, bytes, firstBlock, lastBlock; };
struct Snapshot {
  Row rows[128]; uint64_t calls, bytes, overflowCalls, overflowBytes;
  uint64_t contentionCalls, contentionBytes;
};
class Table {
  unsigned lock = 0;
  Snapshot data = {};
  uint64_t contentionCalls = 0, contentionBytes = 0;
public:
  bool record(Key key, uint64_t bytes, uint64_t block) {
    if (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_add(&contentionCalls, 1ULL, __ATOMIC_RELAXED);
      __atomic_fetch_add(&contentionBytes, bytes, __ATOMIC_RELAXED); return false;
    }
    ++data.calls; data.bytes += bytes;
    uint64_t hash = key.tid ^ (uint64_t(key.pid) << 32) ^ (uint64_t(key.window) << 16) ^ (key.flags << 3) ^ key.size;
    hash ^= hash >> 33; hash *= 0xff51afd7ed558ccdULL; hash ^= hash >> 33;
    unsigned start = unsigned(hash) & 127;
    bool found = false;
    for (unsigned probe = 0; probe < 8; ++probe) {
      Row &row = data.rows[(start + probe) & 127];
      if (!row.used) { row = {}; row.used = true; row.key = key; row.firstBlock = block; }
      if (row.key.pid == key.pid && row.key.window == key.window && row.key.flags == key.flags && row.key.size == key.size && row.key.tid == key.tid) {
        ++row.calls; row.bytes += bytes; row.lastBlock = block; found = true; break;
      }
    }
    if (!found) { ++data.overflowCalls; data.overflowBytes += bytes; }
    __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE); return found;
  }
  bool drain(Snapshot &out) {
    if (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) return false;
    out = data; __builtin_memset(&data, 0, sizeof(data));
    __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE);
    // These two values are independently sampled, not an atomic pair.
    out.contentionCalls = __atomic_exchange_n(&contentionCalls, 0ULL, __ATOMIC_RELAXED);
    out.contentionBytes = __atomic_exchange_n(&contentionBytes, 0ULL, __ATOMIC_RELAXED);
    return true;
  }
};
}
