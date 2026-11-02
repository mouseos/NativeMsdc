// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the boot log window placement of NativeMsdc.h (native-msdc-log): the window is the last blocks of
// userdata, inside the store reserve, and every refusal names its reason.
// Build and run: tests/run.sh
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../NativeMsdc.h"

using namespace NativeMsdcHw;

int main ()
{
  uint64_t first = 0;
  // a04br3 userdata: LBA 557056..30501887 (29944832 sectors, 3743104 blocks of 4 KiB); 1 GiB reserve, 8 MiB log.
  const uint64_t sectors = 30501887 - 557056 + 1, reserve = 1024 * 256, blocks = 2048;
  assert (logWindow (sectors, reserve, blocks, first) == nullptr);
  assert (first == sectors / kSectorsPerStoreBlock - blocks);
  assert ((first + blocks) * kSectorsPerStoreBlock == sectors);     // ends exactly at userdataLast
  // The window lies after the store, which ends where the reserve begins.
  uint64_t storeFirst = 0;
  assert (storeWindow (sectors, uint64_t (8) << 30, reserve, 4096, storeFirst) == nullptr);
  assert (storeFirst + 4096 <= first);
  // Refusals.
  assert (std::strcmp (logWindow (sectors, reserve, 0, first), "size") == 0);
  assert (std::strcmp (logWindow (sectors + 1, reserve, blocks, first), "size") == 0);
  assert (std::strcmp (logWindow (sectors, blocks - 1, blocks, first), "outside the store reserve") == 0);
  assert (std::strcmp (logWindow (sectors, sectors, blocks, first), "outside the store reserve") == 0);
  std::puts ("boot log window: ok");
  return 0;
}

