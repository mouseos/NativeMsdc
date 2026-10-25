// SPDX-License-Identifier: BSD-2-Clause-Patent
#define main verifiedPolicyBaselineMain
#include "VerifiedPolicyTests.cpp"
#undef main

int main ()
{
  FakeOps ops;
  ops.windowBytes = 3 * 65536 + 1536;
  std::vector<uint8_t> bounce (65536, 0xdd);
  VerifiedReadCache cache;
  volatile uint64_t metrics[3] = {};
  cache.metrics = metrics;
  VerifiedReadKey key = { 1, 1000, ops.windowBytes, 1, 7, true };
  auto read = [&](uint64_t sector, uint64_t count) {
    ops.out.assign (count * kSector, 0xee);
    auto result = verifiedReadWindow (ops, ops.windowBytes, sector, count,
                                      bounce.data (), bounce.size (), &cache, &key);
    if (!result.status)
      for (uint64_t i = 0; i < ops.out.size (); ++i) assert (ops.out[i] == imageByte (sector * kSector + i));
    assert (!ops.locked);
    return result;
  };
  assert (!read (0, 128).status && cache.valid && ops.reads == 1 && ops.verifies == 16);
  assert (!read (1, 1).status && ops.reads == 1 && ops.verifies == 16); // unaligned requested slice
  assert (metrics[0] == 1 && metrics[1] == 1 && metrics[2] == 512);
  assert (!read (127, 2).status && ops.reads == 2 && ops.verifies == 17); // hit then cross cached range
  assert (metrics[0] == 2 && metrics[1] == 2 && metrics[2] == 1024);
  // Every identity component independently prevents reuse.
  for (unsigned which = 0; which < 5; ++which) {
    unsigned before = ops.reads;
    switch (which) {
      case 0: ++key.window; break;
      case 1: ++key.firstSector; break;
      case 2: ++key.configuration; break;
      case 3: ++key.generation; break;
      case 4: cache.invalidate (); break;
    }
    assert (!read (128, 1).status && ops.reads == before + 1);
  }
  // Excluded Data/overlay/writable/default-off caller never reuses or retains bytes.
  key.eligible = false;
  unsigned before = ops.reads;
  uint64_t hits = metrics[0], misses = metrics[1];
  assert (!read (128, 1).status && !read (128, 1).status && ops.reads == before + 2 && !cache.valid);
  assert (metrics[0] == hits && metrics[1] == misses);
  key.eligible = true;
  // A window-size key mismatch excludes reuse even if range bytes overlap.
  ++key.windowBytes;
  before = ops.reads;
  assert (read (128, 1).status == 2 && ops.reads == before && !cache.valid);
  --key.windowBytes;
  // A hash failure after overwrite cannot leave an older entry, including a partially verified chunk.
  ops.corruptBlock = 17;
  auto failed = read (128, 16);
  assert (failed.status == 4 && !failed.done && !cache.valid);
  for (auto b : ops.out) assert (b == 0xee);
  ops.corruptBlock = ~0ULL;
  assert (!read (128, 16).status && cache.valid);
  ops.readFails = true;
  failed = read (256, 1);
  assert (failed.status == 3 && !cache.valid);
  ops.readFails = false;
  // Short image tail is authenticated/cached only to its real end, never padding.
  assert (!read (384, 3).status && cache.length == 1536);
  before = ops.reads;
  assert (!read (386, 1).status && ops.reads == before);
  assert (read (386, 2).status == 1);
  auto invalid = verifiedReadWindow (ops, ops.windowBytes, ~0ULL, 1, bounce.data (), bounce.size (), &cache, &key);
  assert (invalid.status == 1);
  // A different backing buffer cannot inherit the entry.
  std::vector<uint8_t> other (65536, 0xcc);
  ops.out.assign (512, 0xee); before = ops.reads;
  assert (!verifiedReadWindow (ops, ops.windowBytes, 386, 1, other.data (), other.size (), &cache, &key).status);
  assert (ops.reads == before + 1 && cache.storage == other.data ());
  for (uint64_t i = 0; i < ops.out.size (); ++i) assert (ops.out[i] == imageByte (386 * kSector + i));
  // A short external copy on a hit fails and invalidates; no fake completion.
  struct ShortCopy : FakeOps {
    bool shortCopy = false;
    uint64_t copy (uint64_t offset, const uint8_t *data, uint64_t length) {
      return FakeOps::copy (offset, data, shortCopy ? length - 1 : length);
    }
  } shortOps;
  shortOps.windowBytes = 65536;
  shortOps.out.resize (512);
  VerifiedReadCache shortCache;
  VerifiedReadKey shortKey = { 2, 2000, 65536, 1, 8, true };
  assert (!verifiedReadWindow (shortOps, 65536, 0, 1, bounce.data (), bounce.size (), &shortCache, &shortKey).status);
  shortOps.shortCopy = true;
  auto shortResult = verifiedReadWindow (shortOps, 65536, 0, 1, bounce.data (), bounce.size (), &shortCache, &shortKey);
  assert (shortResult.status == 2 && shortResult.done == 511 && !shortCache.valid && shortOps.reads == 1);
  // Near-maximum sizes must calculate a bounded first read without arithmetic wrap.
  ops.windowBytes = ~uint64_t (0) - 511;
  ops.readFails = true;
  key.windowBytes = ops.windowBytes;
  auto huge = verifiedReadWindow (ops, ops.windowBytes, 1, ops.windowBytes / 512 - 1,
                                  bounce.data (), bounce.size (), &cache, &key);
  assert (huge.status == 3 && !huge.done && !cache.valid);
  puts ("NativeMsdc verified cache tests passed (performance unverified)");
}
