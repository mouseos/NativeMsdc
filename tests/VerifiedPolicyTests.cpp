// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the pure signed-root-policy logic of NativeMsdc.h: the policy region location, its header, the
// selection of one policy per window and the verified window read (block ranges, the short last block, failures).
// Build and run: tests/run.sh
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../NativeMsdc.h"

using namespace NativeMsdcHw;

static void put32 (uint8_t *at, uint32_t value) { for (unsigned i = 0; i < 4; ++i) at[i] = uint8_t (value >> (8 * i)); }
static void put64 (uint8_t *at, uint64_t value) { for (unsigned i = 0; i < 8; ++i) at[i] = uint8_t (value >> (8 * i)); }

// A region with the given entries; each envelope only carries its image size at byte 24 (enough for selection).
struct Region {
  std::vector<uint8_t> bytes;
  Region (const std::vector<PolicyEntry> &entries, uint64_t total, const std::vector<uint64_t> &images = {})
    : bytes (total, 0)
  {
    memcpy (bytes.data (), "A04RPOL1", 8);
    put32 (&bytes[8], 1);
    put32 (&bytes[12], uint32_t (entries.size ()));
    put64 (&bytes[16], total);
    for (size_t i = 0; i < entries.size (); ++i) {
      put64 (&bytes[24 + 16 * i], entries[i].offset);
      put64 (&bytes[32 + 16 * i], entries[i].bytes);
      if (i < images.size () && entries[i].offset + 32 <= total) put64 (&bytes[entries[i].offset + 24], images[i]);
    }
    for (unsigned i = 0; i < 32; ++i) bytes[kPolicyHashed + i] = uint8_t (0xa0 + i);   // stands for the header SHA-256
  }
  const uint8_t *digest () const { return bytes.data () + kPolicyHashed; }
  unsigned parse (PolicyHeader &policy, uint64_t available = kPolicyMaxBytes) const
  {
    return parsePolicyHeader (bytes.data (), digest (), available, policy);
  }
};

static void testLocation ()
{
  RootManifest manifest = {};
  manifest.count = 2;
  manifest.window[0] = { "OS", 4096, 6077580800ULL, 1, {} };              // ends at 6077584896
  manifest.window[1] = { "SystemOS", 1048576ULL * 5000, 512 * 3, 1, {} };  // ends earlier
  assert (policyRegionOffset (manifest) == 5797ULL * 1048576);            // 6077584896 rounded up to 1 MiB
  manifest.window[1] = { "SystemOS", 1048576ULL * 5900, 1048576ULL * 100, 1, {} };   // ends on 6000 MiB
  assert (policyRegionOffset (manifest) == 6000ULL * 1048576);            // already aligned: unchanged
}

static void testHeader ()
{
  PolicyHeader policy;
  Region good ({{4096, 1000}, {8192, 2000}}, 16384);
  assert (good.parse (policy) == kPolicyOk && policy.count == 2 && policy.totalBytes == 16384);
  assert (policy.entry[1].offset == 8192 && policy.entry[1].bytes == 2000);

  Region magic = good;
  magic.bytes[7] = '2';
  assert (magic.parse (policy) == kPolicyNoMagic);
  Region version = good;
  put32 (&version.bytes[8], 2);
  assert (version.parse (policy) == kPolicyVersion);
  Region none = good;
  put32 (&none.bytes[12], 0);
  assert (none.parse (policy) == kPolicyVersion);
  Region many = good;
  put32 (&many.bytes[12], 17);
  assert (many.parse (policy) == kPolicyVersion);
  uint8_t wrong[32];
  memcpy (wrong, good.digest (), 32);
  wrong[31] ^= 1;
  assert (parsePolicyHeader (good.bytes.data (), wrong, kPolicyMaxBytes, policy) == kPolicyHash);
  assert (good.parse (policy, 16383) == kPolicySize);         // does not fit in userdata
  Region huge ({{4096, 1000}}, 8192);
  put64 (&huge.bytes[16], kPolicyMaxBytes + 1);
  assert (huge.parse (policy, ~0ULL) == kPolicySize);
  Region small ({{4096, 1000}}, 8192);
  put64 (&small.bytes[16], 4095);
  assert (small.parse (policy) == kPolicySize);

  assert (Region ({{0, 1000}}, 8192).parse (policy) == kPolicyEntryBounds);          // inside the header
  assert (Region ({{4096, 100}}, 8192).parse (policy) == kPolicyEntryBounds);        // shorter than an envelope
  assert (Region ({{4096, 4097}}, 8192).parse (policy) == kPolicyEntryBounds);       // past total_bytes
  assert (Region ({{4096, ~0ULL - 100}}, 8192).parse (policy) == kPolicyEntryBounds);
  assert (Region ({{4096, 1000}, {5000, 1000}}, 8192).parse (policy) == kPolicyEntryOverlap);
  assert (Region ({{4096, 1000}, {5096, 1000}}, 8192).parse (policy) == kPolicyOk);  // adjacent
}

static void testSelection ()
{
  PolicyHeader policy;
  Region region ({{4096, 1000}, {8192, 1000}, {12288, 1000}}, 16384, {6077580800ULL, 1048576, 1048576});
  assert (region.parse (policy) == kPolicyOk);
  unsigned index = 99;
  assert (selectPolicy (region.bytes.data (), policy, 6077580800ULL, index) == kPolicySelected && index == 0);
  assert (selectPolicy (region.bytes.data (), policy, 1048576, index) == kPolicyDuplicate);
  assert (selectPolicy (region.bytes.data (), policy, 4096, index) == kPolicyNoMatch);
}

// ---- Verified read --------------------------------------------------------------------------------------------------
static uint8_t imageByte (uint64_t position) { return uint8_t ((position * 2654435761ULL) >> 13); }

struct FakeOps {
  uint64_t windowBytes = 0;
  uint64_t corruptBlock = ~0ULL;      // the device returns this block with one byte changed
  bool readFails = false;
  std::vector<uint8_t> out;
  uint64_t reads = 0, verifies = 0, maxSectors = 0;
  bool locked = false;
  std::vector<uint64_t> verifiedBlocks;
  void lock () { assert (!locked); locked = true; }
  void unlock () { assert (locked); locked = false; }
  int badArgument () { return 1; }
  int ioError () { return 2; }
  int read (uint64_t sector, uint32_t sectors, uint8_t *buffer)
  {
    assert (locked && sectors && sector * kSector + uint64_t (sectors) * kSector <= windowBytes);
    assert (sector % (kVerifiedBlock / kSector) == 0);
    ++reads;
    if (sectors > maxSectors) maxSectors = sectors;
    if (readFails) return 3;
    for (uint64_t i = 0; i < uint64_t (sectors) * kSector; ++i) {
      uint64_t position = sector * kSector + i;
      buffer[i] = imageByte (position) ^ uint8_t (position / kVerifiedBlock == corruptBlock && position % kVerifiedBlock == 7);
    }
    return 0;
  }
  int verify (uint64_t index, const uint8_t *data, size_t length)
  {
    assert (locked);
    uint64_t base = index * kVerifiedBlock;
    size_t expected = windowBytes - base < kVerifiedBlock ? size_t (windowBytes - base) : size_t (kVerifiedBlock);
    assert (length == expected);
    ++verifies;
    verifiedBlocks.push_back (index);
    for (size_t i = 0; i < length; ++i)
      if (data[i] != imageByte (base + i)) return 4;    // stands for kIOReturnNotPrivileged
    return 0;
  }
  uint64_t copy (uint64_t offset, const uint8_t *data, uint64_t length)
  {
    assert (locked && offset + length <= out.size ());
    memcpy (out.data () + offset, data, length);
    return length;
  }
};

static void checkRead (uint64_t windowBytes, uint64_t block, uint64_t count, uint64_t bounceBytes = 65536)
{
  FakeOps ops;
  ops.windowBytes = windowBytes;
  ops.out.assign (count * kSector, 0xee);
  std::vector<uint8_t> bounce (bounceBytes);
  VerifiedReadResult result = verifiedReadWindow (ops, windowBytes, block, count, bounce.data (), bounceBytes);
  assert (result.status == 0 && result.done == count * kSector && !ops.locked);
  for (uint64_t i = 0; i < count * kSector; ++i) assert (ops.out[i] == imageByte (block * kSector + i));
  uint64_t firstBlock = block * kSector / kVerifiedBlock;
  uint64_t lastBlock = ((block + count) * kSector - 1) / kVerifiedBlock;
  assert (ops.verifies == lastBlock - firstBlock + 1);         // every touched block verified exactly once
  for (size_t i = 0; i < ops.verifiedBlocks.size (); ++i) assert (ops.verifiedBlocks[i] == firstBlock + i);
  assert (ops.maxSectors <= bounceBytes / kSector);
}

static void testRead ()
{
  const uint64_t os = 6077580800ULL;                 // the OS window: last block 1536 bytes (3 sectors)
  assert (os % kVerifiedBlock == 1536);
  const uint64_t sectors = os / kSector;
  checkRead (os, 0, 1);
  checkRead (os, 0, 8);
  checkRead (os, 3, 2);                               // inside one block
  checkRead (os, 7, 2);                               // across a block boundary
  checkRead (os, 1, 300);                             // several chunks, unaligned both ends
  checkRead (os, sectors - 1, 1);                     // the very last sector
  checkRead (os, sectors - 3, 3);                     // exactly the short last block
  checkRead (os, sectors - 11, 11);                   // the last full block and the short one
  checkRead (os, sectors - 200, 200);                 // chunk ends inside the short block
  checkRead (os, sectors - 128, 128, 4096);           // one block per chunk
  checkRead (8192, 0, 16);                            // whole window, aligned
  checkRead (512, 0, 1);                              // a window smaller than a block

  // Out of range or bad geometry.
  FakeOps ops;
  ops.windowBytes = os;
  ops.out.assign (4096, 0);
  std::vector<uint8_t> bounce (65536);
  assert (verifiedReadWindow (ops, os, sectors, 1, bounce.data (), 65536).status == 1);
  assert (verifiedReadWindow (ops, os, sectors - 1, 2, bounce.data (), 65536).status == 1);
  assert (verifiedReadWindow (ops, os, 0, 0, bounce.data (), 65536).status == 1);
  assert (verifiedReadWindow (ops, os + 1, 0, 1, bounce.data (), 65536).status == 1);
  assert (verifiedReadWindow (ops, os, 0, 1, bounce.data (), 1000).status == 1);
  assert (ops.reads == 0);

  // A corrupted block refuses the read: nothing from that chunk is copied.
  FakeOps bad;
  bad.windowBytes = os;
  bad.corruptBlock = 20;                              // in the second 64 KiB chunk
  bad.out.assign (40 * 4096, 0xee);
  VerifiedReadResult result = verifiedReadWindow (bad, os, 0, 40 * 8, bounce.data (), 65536);
  assert (result.status == 4 && result.done == 65536 && !bad.locked);
  for (uint64_t i = 65536; i < bad.out.size (); ++i) assert (bad.out[i] == 0xee);
  FakeOps last;
  last.windowBytes = os;
  last.corruptBlock = os / kVerifiedBlock;            // the short last block
  last.out.assign (512, 0);
  assert (verifiedReadWindow (last, os, sectors - 1, 1, bounce.data (), 65536).status == 4);

  FakeOps fails;
  fails.windowBytes = os;
  fails.readFails = true;
  fails.out.assign (512, 0);
  result = verifiedReadWindow (fails, os, 0, 1, bounce.data (), 65536);
  assert (result.status == 3 && result.done == 0 && fails.verifies == 0 && !fails.locked);
}

int main ()
{
  testLocation ();
  testHeader ();
  testSelection ();
  testRead ();
  puts ("NativeMsdc verified policy tests passed");
  return 0;
}
