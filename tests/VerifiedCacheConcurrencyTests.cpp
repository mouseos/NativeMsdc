// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the actual verifiedReadWindow template. No XNU lock, DMA or device is simulated.
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "../NativeMsdc.h"
using namespace NativeMsdcHw;

namespace {
constexpr uint64_t kWindowBytes = 1u << 20;
uint8_t byteAt(uint64_t offset, uint64_t generation) { return uint8_t((offset * 37 + (offset >> 8) + generation * 13) & 255); }
struct Shared {
  std::mutex lock;
  uint64_t generation = 1;
  VerifiedReadCache cache;
  std::array<uint8_t, 65536> bounce = {};
  volatile uint64_t metrics[3] = {};
  Shared() { cache.metrics = metrics; }
};
struct Operations {
  Shared &shared;
  VerifiedReadKey &key;
  uint64_t requestStart;
  uint64_t generation = 0;
  bool held = false;
  void lock() {
    shared.lock.lock(); held = true; generation = shared.generation;
    key = {1, 1000, kWindowBytes, generation, generation, true};
  }
  void unlock() { assert(held); held = false; shared.lock.unlock(); }
  int badArgument() { return 1; }
  int ioError() { return 2; }
  int read(uint64_t sector, uint32_t blocks, uint8_t *out) {
    assert(held);
    for (uint64_t i = 0; i < uint64_t(blocks) * kSector; ++i) out[i] = byteAt(sector * kSector + i, generation);
    return 0;
  }
  int verify(uint64_t index, const uint8_t *data, size_t length) {
    assert(held);
    for (size_t i = 0; i < length; ++i) assert(data[i] == byteAt(index * kVerifiedBlock + i, generation));
    return 0;
  }
  uint64_t copy(uint64_t offset, const uint8_t *data, uint64_t length) {
    assert(held && generation == shared.generation);
    for (uint64_t i = 0; i < length; ++i) assert(data[i] == byteAt(requestStart + offset + i, generation));
    return length;
  }
};
void read(Shared &s, uint64_t block, uint64_t count) {
  VerifiedReadKey key = {};
  Operations op{s, key, block * kSector};
  const auto result = verifiedReadWindow(op, kWindowBytes, block, count, s.bounce.data(), s.bounce.size(), &s.cache, &key);
  assert(result.status == 0 && result.done == count * kSector && !op.held);
}
}
int main() {
  Shared s;
  read(s, 32, 32); read(s, 33, 1);
  assert(s.metrics[0] == 1 && s.metrics[1] == 1);
  std::vector<std::thread> threads;
  for (uint64_t t = 0; t < 6; ++t) threads.emplace_back([&, t] {
    for (uint64_t i = 0; i < 1800; ++i) {
      const uint64_t b = ((i * 13 + t * 107) % ((kWindowBytes / kSector) - 32));
      read(s, b, 1 + (i % 32));
    }
  });
  threads.emplace_back([&] {
    for (unsigned i = 0; i < 2000; ++i) {
      std::lock_guard<std::mutex> guard(s.lock);
      s.cache.invalidate(); ++s.generation;
    }
  });
  for (auto &thread : threads) thread.join();
  assert(s.metrics[0] > 0 && s.metrics[1] > 0);
  std::puts("PASS verifiedReadWindow pthread: 10800 reads, concurrent generation/invalidation, byte and verification checks; host model only");
}
