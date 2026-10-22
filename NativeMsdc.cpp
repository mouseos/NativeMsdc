// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeMsdc: eMMC block storage on the MediaTek MT8167 MSDC (MSDC0 of the Benesse TAB-A04-BR3).
//
// Default-off: the personality matches the "mediatek,mt8167-mmc" node, but nothing touches the controller unless
// native-msdc-root or native-msdc-persist is set. The controller is inherited from the boot firmware as it was
// left (card in the transfer state, bus width, clock and sampling unchanged); this driver never resets or retunes it.
// Every transaction masks the controller interrupts and selects PIO, then restores both, so firmware services that
// share the controller see it as they left it. Commands are polled.
//
//   native-msdc-root=1      publish the windows of the SHNGROOT manifest in userdata as read-only block devices
//   native-msdc-root=2      as 1, and windows whose manifest entry clears the read-only flag accept writes
//   native-msdc-persist=1|2 persistent Data store at the end of userdata (1 proves it read-only, 2 opens it for writes)
//   native-msdc-reserve=MiB reserve at the end of userdata that the store stays clear of (default 1024)
//   native-msdc-dma=1       descriptor DMA for reads, enabled only after it reads the same bytes as PIO
//   native-msdc-verify=1    hash every window against its manifest SHA-256 before publishing it
//
// Writes are bounded multi-block writes (CMD23 + CMD25) that return only when the card is back in the transfer
// state. Forced unit access (the store, and window writes that ask for it) adds a FLUSH_CACHE when the card cache
// is on, so the data is on the medium when the request completes.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "NativeMsdc.h"

using namespace NativeMsdcHw;

extern "C" {
// Boot diagnostics: 1 started, 2 card ready, 3 userdata found, 4 manifest valid, 5 windows scheduled, 6 windows published.
volatile uint32_t native_msdc_step = 0;
const char *volatile native_msdc_failure = nullptr;
volatile uint32_t native_msdc_manifest_status = 0xffffffff;
volatile uint32_t native_msdc_dma = 0;                      // 0 PIO, 1 DMA verified, 2 DMA rejected, 3 quarantined
volatile uint32_t native_msdc_blocks_published = 0;
volatile uint64_t native_msdc_reads = 0, native_msdc_writes = 0, native_msdc_flushes = 0, native_msdc_failures = 0;
}

namespace {

constexpr uint64_t kCommandTimeoutNs = 1000000000ULL;     // one command and its data phase
constexpr uint64_t kBusyTimeoutNs = 5000000000ULL;        // programming after a write, cache flush
constexpr unsigned kDmaDescriptorBytes = 4096;             // GPD at 0, BD at 64; the buffer follows
constexpr unsigned kTransferBytes = kMaxBlocks * kSector;
constexpr uint64_t kDmaPhysicalMask = 0x00000000fffff000ULL;
constexpr uint32_t kDmaSelfTestSector = 0x40;
constexpr unsigned kDmaSelfTestBlocks = 4;

struct MsdcHost {
  IOMemoryMap *map;
  uintptr_t base;
  IOLock *lock;
  uint8_t *bounce;            // kTransferBytes, used under lock
  uint32_t diskSectors;
  bool cache;                 // EXT_CSD CACHE_CTRL
  bool ready;
  bool poisoned;              // the DMA engine did not stop: no further transfers
  IOBufferMemoryDescriptor *dma;
  uint8_t *dmaVirt;
  uint64_t dmaPhys;
  bool dmaEnabled;
  GptLayout gpt;
  bool layoutValid;
  RootManifest manifest;
  bool manifestValid;
};
MsdcHost gHost;

inline uint32_t rd (unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (gHost.base + offset);
}

inline void wr (unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (gHost.base + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

uint64_t deadlineAfter (uint64_t nanoseconds)
{
  uint64_t ticks = 0;
  nanoseconds_to_absolutetime (nanoseconds, &ticks);
  return mach_absolute_time () + ticks;
}

IOReturn waitClear (unsigned offset, uint32_t mask)
{
  uint64_t end = deadlineAfter (kCommandTimeoutNs);
  do {
    if (!(rd (offset) & mask)) return kIOReturnSuccess;
  } while (mach_absolute_time () < end);
  return kIOReturnTimeout;
}

void cacheCleanInvalidate (const void *start, size_t length)
{
  uint64_t ctr;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
  uintptr_t line = uintptr_t (4) << ((ctr >> 16) & 15);
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~(line - 1);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += line) __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  __asm__ volatile ("dsb sy" ::: "memory");
}

// ---- Controller access (gHost.lock held, inside a HostSession) -------------------------------------------------------

IOReturn hostCommand (unsigned opcode, uint32_t argument, Data data, uint32_t *response)
{
  IOReturn result = waitClear (kSdcSts, busyMask (opcode));
  if (result != kIOReturnSuccess) return result;
  wr (kMsdcIntr, rd (kMsdcIntr));
  wr (kSdcArg, argument);
  wr (kSdcCmd, commandWord (opcode, data));
  uint64_t end = deadlineAfter (kCommandTimeoutNs);
  do {
    uint32_t status = rd (kMsdcIntr) & (kIntCmdReady | kIntCmdErrors);
    if (status) {
      wr (kMsdcIntr, status);
      if (status & kIntCmdErrors) return kIOReturnIOError;
      *response = rd (kSdcResp0);
      return (*response & kR1Errors) ? kIOReturnIOError : kIOReturnSuccess;
    }
  } while (mach_absolute_time () < end);
  return kIOReturnTimeout;
}

// SEND_STATUS until the card is back in the transfer state (after programming, or an R1b command).
IOReturn hostWaitTransfer (void)
{
  uint64_t end = deadlineAfter (kBusyTimeoutNs);
  do {
    uint32_t response = 0;
    IOReturn result = hostCommand (kCmdStatus, kStatusArgument, Data::None, &response);
    if (result != kIOReturnSuccess) return result;
    if (cardInTransfer (response)) return kIOReturnSuccess;
  } while (mach_absolute_time () < end);
  return kIOReturnTimeout;
}

IOReturn hostPrepareData (uint32_t blocks)
{
  wr (kSdcBlkNum, blocks);
  wr (kMsdcFifoCs, rd (kMsdcFifoCs) | kFifoClear);
  return waitClear (kMsdcFifoCs, kFifoClear);
}

// PIO read. Multi-block reads are bounded by SET_BLOCK_COUNT, so a completed transfer needs no STOP; an interrupted one
// is stopped and its original error kept.
IOReturn hostReadPio (unsigned opcode, uint32_t argument, uint8_t *buffer, uint32_t blocks)
{
  if (!blocks || blocks > kMaxBlocks) return kIOReturnBadArgument;
  uint32_t response = 0;
  bool multi = blocks > 1;
  IOReturn result;
  if (multi) {
    result = hostCommand (kCmdSetBlockCount, blocks, Data::None, &response);
    if (result != kIOReturnSuccess) return result;
    opcode = kCmdReadMulti;
  }
  result = hostPrepareData (blocks);
  if (result != kIOReturnSuccess) return result;
  result = hostCommand (opcode, argument, multi ? Data::ReadMulti : Data::ReadSingle, &response);
  if (result == kIOReturnSuccess) {
    uint64_t end = deadlineAfter (kCommandTimeoutNs);
    unsigned offset = 0, total = blocks * kSector;
    bool done = false;
    result = kIOReturnTimeout;
    do {
      uint32_t interrupts = rd (kMsdcIntr) & (kIntXferDone | kIntDataErrors);
      if (interrupts) {
        wr (kMsdcIntr, interrupts);
        if (interrupts & kIntDataErrors) {
          result = kIOReturnIOError;
          break;
        }
        if (interrupts & kIntXferDone) done = true;
      }
      unsigned available = rd (kMsdcFifoCs) & 255u;
      while (available && offset < total) {
        if (available >= 4 && total - offset >= 4) {
          uint32_t word = rd (kMsdcRxData);
          memcpy (buffer + offset, &word, 4);
          offset += 4;
          available -= 4;
        } else {
          buffer[offset++] = *reinterpret_cast<volatile uint8_t *> (gHost.base + kMsdcRxData);
          available--;
        }
      }
      if (done && offset == total) return kIOReturnSuccess;
    } while (mach_absolute_time () < end);
  }
  if (multi) (void) hostCommand (kCmdStop, 0, Data::None, &response);
  return result;
}

// Descriptor DMA read into the private bounce buffer, then a copy out. The buffer is never reused while the engine
// may still write to it: if the engine does not stop, the transport is quarantined and the memory kept.
IOReturn hostReadDma (uint32_t argument, uint8_t *buffer, uint32_t blocks)
{
  if (!gHost.dmaVirt || !blocks || blocks > kMaxBlocks) return kIOReturnBadArgument;
  unsigned bytes = blocks * kSector;
  DmaGpd *gpd = reinterpret_cast<DmaGpd *> (gHost.dmaVirt);
  DmaBd *bd = reinterpret_cast<DmaBd *> (gHost.dmaVirt + 64);
  uint8_t *data = gHost.dmaVirt + kDmaDescriptorBytes;
  if (!dmaDescriptors (gpd, bd, gHost.dmaPhys, gHost.dmaPhys + 64, gHost.dmaPhys + kDmaDescriptorBytes, bytes))
    return kIOReturnBadArgument;
  cacheCleanInvalidate (gHost.dmaVirt, kDmaDescriptorBytes);
  cacheCleanInvalidate (data, bytes);
  uint32_t response = 0;
  bool multi = blocks > 1;
  IOReturn result = kIOReturnSuccess;
  if (multi) {
    result = hostCommand (kCmdSetBlockCount, blocks, Data::None, &response);
    if (result != kIOReturnSuccess) return result;
  }
  result = hostPrepareData (blocks);
  if (result != kIOReturnSuccess) return result;
  wr (kMsdcCfg, rd (kMsdcCfg) & ~kCfgPio);
  result = hostCommand (multi ? kCmdReadMulti : kCmdReadSingle, argument, multi ? Data::ReadMulti : Data::ReadSingle,
                        &response);
  if (result == kIOReturnSuccess) {
    wr (kDmaSaHigh, 0);
    wr (kDmaSa, uint32_t (gHost.dmaPhys));
    wr (kDmaLength, 0);
    wr (kDmaCfg, rd (kDmaCfg) | kDmaCfgChecksum);
    wr (kDmaCtrl, (rd (kDmaCtrl) & ~(kDmaCtrlStart | kDmaCtrlStop | kDmaCtrlLastBuffer | kDmaCtrlBurstMask)) |
                  kDmaCtrlDescriptor | kDmaCtrlBurst64);
    wr (kDmaCtrl, rd (kDmaCtrl) | kDmaCtrlStart);
    uint64_t end = deadlineAfter (kCommandTimeoutNs);
    for (;;) {
      uint32_t interrupts = rd (kMsdcIntr) & (kIntXferDone | kIntDataErrors | kIntDmaErrors);
      if (interrupts) {
        wr (kMsdcIntr, interrupts);
        if (interrupts & (kIntDataErrors | kIntDmaErrors)) {
          result = kIOReturnIOError;
          break;
        }
        if (interrupts & kIntXferDone) break;
      }
      if (mach_absolute_time () >= end) {
        result = kIOReturnTimeout;
        break;
      }
    }
  }
  wr (kDmaCtrl, rd (kDmaCtrl) | kDmaCtrlStop);
  if (waitClear (kDmaCfg, kDmaCfgActive) != kIOReturnSuccess) {
    gHost.poisoned = true;
    gHost.dmaEnabled = false;
    native_msdc_dma = 3;
    native_msdc_failure = "DMA engine did not stop";
    IOLog ("NativeMsdc: DMA engine did not stop; transport quarantined\n");
    return kIOReturnNotReady;
  }
  wr (kDmaCfg, rd (kDmaCfg) & ~kDmaCfgChecksum);
  wr (kMsdcCfg, rd (kMsdcCfg) | kCfgPio);
  if (result != kIOReturnSuccess) {
    if (multi) (void) hostCommand (kCmdStop, 0, Data::None, &response);
    return result;
  }
  result = waitClear (kSdcSts, kStsSdcBusy);
  if (result != kIOReturnSuccess) return result;
  cacheCleanInvalidate (data, bytes);
  memcpy (buffer, data, bytes);
  return kIOReturnSuccess;
}

// Bounded PIO write (CMD24, or CMD23 + CMD25). Fills each FIFO burst without a barrier per word and returns once the
// card has finished programming.
IOReturn hostWrite (uint32_t sector, const uint8_t *buffer, uint32_t blocks)
{
  if (!blocks || blocks > kMaxBlocks) return kIOReturnBadArgument;
  uint32_t response = 0;
  bool multi = blocks > 1;
  IOReturn result;
  if (multi) {
    result = hostCommand (kCmdSetBlockCount, blocks, Data::None, &response);
    if (result != kIOReturnSuccess) return result;
  }
  result = hostPrepareData (blocks);
  if (result != kIOReturnSuccess) return result;
  result = hostCommand (multi ? kCmdWriteMulti : kCmdWriteSingle, sector, multi ? Data::WriteMulti : Data::WriteSingle,
                        &response);
  if (result == kIOReturnSuccess) {
    uint64_t end = deadlineAfter (kCommandTimeoutNs);
    unsigned offset = 0, total = blocks * kSector;
    bool done = false;
    result = kIOReturnTimeout;
    do {
      uint32_t interrupts = rd (kMsdcIntr) & (kIntXferDone | kIntDataErrors);
      if (interrupts) {
        wr (kMsdcIntr, interrupts);
        if (interrupts & kIntDataErrors) {
          result = kIOReturnIOError;
          break;
        }
        if (interrupts & kIntXferDone) done = true;
      }
      unsigned used = (rd (kMsdcFifoCs) >> 16) & 255u;
      unsigned room = used <= kFifoBytes ? kFifoBytes - used : 0;
      while (room >= 4 && offset < total) {
        uint32_t word;
        memcpy (&word, buffer + offset, 4);
        *reinterpret_cast<volatile uint32_t *> (gHost.base + kMsdcTxData) = word;
        offset += 4;
        room -= 4;
      }
      __asm__ volatile ("dsb sy" ::: "memory");
      if (done && offset == total) {
        result = hostWaitTransfer ();
        if (result == kIOReturnSuccess) return kIOReturnSuccess;
        break;
      }
    } while (mach_absolute_time () < end);
  }
  if (multi) (void) hostCommand (kCmdStop, 0, Data::None, &response);
  return result;
}

IOReturn hostFlush (void)
{
  if (!gHost.cache) return kIOReturnSuccess;
  uint32_t response = 0;
  IOReturn result = hostCommand (kCmdSwitch, kFlushCacheArgument, Data::None, &response);
  if (result == kIOReturnSuccess) result = hostWaitTransfer ();
  if (result == kIOReturnSuccess) __atomic_fetch_add (&native_msdc_flushes, 1, __ATOMIC_RELAXED);
  return result;
}

// Holds the controller for one request: interrupts masked and PIO selected, then the firmware's values restored.
class HostSession {
  uint32_t cfg = 0, interruptEnable = 0;
public:
  HostSession ()
  {
    IOLockLock (gHost.lock);
    cfg = rd (kMsdcCfg);
    interruptEnable = rd (kMsdcIntEn);
    wr (kMsdcIntEn, 0);
    wr (kMsdcCfg, cfg | kCfgPio);
  }
  ~HostSession ()
  {
    wr (kMsdcIntr, rd (kMsdcIntr));
    wr (kMsdcCfg, cfg);
    wr (kMsdcIntEn, interruptEnable);
    IOLockUnlock (gHost.lock);
  }
  HostSession (const HostSession &) = delete;
  HostSession &operator= (const HostSession &) = delete;
};

// One transfer of at most kMaxBlocks sectors anywhere on the user area; callers bound it to their own region.
IOReturn transfer (uint64_t sector, uint32_t blocks, uint8_t *buffer, bool write)
{
  if (gHost.poisoned) return kIOReturnNotReady;
  if (!blocks || blocks > kMaxBlocks || sector >= gHost.diskSectors || blocks > gHost.diskSectors - sector)
    return kIOReturnBadArgument;
  IOReturn result;
  if (write) result = hostWrite (uint32_t (sector), buffer, blocks);
  else if (gHost.dmaEnabled) result = hostReadDma (uint32_t (sector), buffer, blocks);
  else result = hostReadPio (kCmdReadSingle, uint32_t (sector), buffer, blocks);
  if (result != kIOReturnSuccess) __atomic_fetch_add (&native_msdc_failures, 1, __ATOMIC_RELAXED);
  else __atomic_fetch_add (write ? &native_msdc_writes : &native_msdc_reads, 1, __ATOMIC_RELAXED);
  return result;
}

unsigned bootArgument (const char *name, unsigned maximum, unsigned fallback)
{
  unsigned value = fallback;
  if (!PE_parse_boot_argn (name, &value, sizeof (value)) || value > maximum) value = fallback;
  return value;
}

void fail (const char *why)
{
  native_msdc_failure = why;
  IOLog ("NativeMsdc: %s\n", why);
}

// ---- Bring-up --------------------------------------------------------------------------------------------------------

bool listHas (OSObject *property, const char *wanted)
{
  const char *bytes = nullptr;
  size_t length = 0;
  if (OSData *data = OSDynamicCast (OSData, property)) {
    bytes = static_cast<const char *> (data->getBytesNoCopy ());
    length = data->getLength ();
  } else if (OSString *string = OSDynamicCast (OSString, property)) {
    bytes = string->getCStringNoCopy ();
    length = string->getLength ();
  }
  size_t wantedLength = strlen (wanted);
  for (size_t at = 0; bytes && at < length;) {
    size_t end = at;
    while (end < length && bytes[end]) ++end;
    if (end - at == wantedLength && !memcmp (bytes + at, wanted, wantedLength)) return true;
    at = end + 1;
  }
  return false;
}

uint32_t cellProperty (IORegistryEntry *entry, const char *name)
{
  OSData *data = entry ? OSDynamicCast (OSData, entry->getProperty (name)) : nullptr;
  return data && data->getLength () == 4 ? le32 (static_cast<const uint8_t *> (data->getBytesNoCopy ())) : 0;
}

// The register window: the platform expert's resolution of reg (getDeviceMemoryWithIndex), cross-checked against reg
// and the arm-io ranges read directly when both use 2 address and 2 size cells. A disagreement is refused.
bool controllerWindow (IOService *provider, uint64_t &phys, uint64_t &length, const char **why)
{
  bool resolved = false, computed = false;
  uint64_t resolvedPhys = 0, resolvedLength = 0, computedPhys = 0, computedLength = 0;
  if (IODeviceMemory *memory = provider->getDeviceMemoryWithIndex (0)) {
    resolvedPhys = memory->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
    resolvedLength = memory->getLength ();
    resolved = true;
  }
  IORegistryEntry *bus = provider->getParentEntry (gIODTPlane);
  IORegistryEntry *root = bus ? bus->getParentEntry (gIODTPlane) : nullptr;
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  OSData *ranges = bus ? OSDynamicCast (OSData, bus->getProperty ("ranges")) : nullptr;
  if (reg && ranges && reg->getLength () >= 16 && ranges->getLength () && !(ranges->getLength () % 24) &&
      cellProperty (bus, "#address-cells") == 2 && cellProperty (bus, "#size-cells") == 2 &&
      cellProperty (root, "#address-cells") == 2) {
    const uint8_t *cells = static_cast<const uint8_t *> (reg->getBytesNoCopy ());
    const uint8_t *range = static_cast<const uint8_t *> (ranges->getBytesNoCopy ());
    uint64_t child = le64 (cells), size = le64 (cells + 8);
    for (unsigned at = 0; at < ranges->getLength (); at += 24) {
      uint64_t childBase = le64 (range + at), parentBase = le64 (range + at + 8), rangeSize = le64 (range + at + 16);
      if (child >= childBase && child - childBase < rangeSize && size <= rangeSize - (child - childBase)) {
        computedPhys = parentBase + (child - childBase);
        computedLength = size;
        computed = true;
        break;
      }
    }
  }
  if (resolved && computed && (resolvedPhys != computedPhys || resolvedLength != computedLength)) {
    *why = "controller address: platform resolution and reg/ranges disagree";
    return false;
  }
  if (!resolved && !computed) {
    *why = "controller address: no usable reg";
    return false;
  }
  phys = resolved ? resolvedPhys : computedPhys;
  length = resolved ? resolvedLength : computedLength;
  if (!phys || (phys & 4095) || length < 0x1000) {
    *why = "controller address: unaligned or short register window";
    return false;
  }
  return true;
}

// Compare DMA with PIO on the same sectors; enable DMA only when they match.
void hostEnableDma (void)
{
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone), kDmaDescriptorBytes + kTransferBytes, kDmaPhysicalMask);
  if (!gHost.dma) {
    native_msdc_dma = 2;
    IOLog ("NativeMsdc: DMA buffer allocation failed; PIO\n");
    return;
  }
  IOByteCount contiguous = 0;
  gHost.dmaVirt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  gHost.dmaPhys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  IOReturn result = kIOReturnNoMemory;
  if (gHost.dmaVirt && gHost.dmaPhys && !(gHost.dmaPhys & 4095) &&
      contiguous >= kDmaDescriptorBytes + kTransferBytes &&
      gHost.dmaPhys + kDmaDescriptorBytes + kTransferBytes <= 0x100000000ULL) {
    bzero (gHost.dmaVirt, kDmaDescriptorBytes + kTransferBytes);
    uint8_t *reference = gHost.bounce, *candidate = gHost.bounce + kDmaSelfTestBlocks * kSector;
    result = hostReadPio (kCmdReadSingle, kDmaSelfTestSector, reference, kDmaSelfTestBlocks);
    if (result == kIOReturnSuccess) result = hostReadDma (kDmaSelfTestSector, candidate, kDmaSelfTestBlocks);
    if (result == kIOReturnSuccess && memcmp (reference, candidate, kDmaSelfTestBlocks * kSector)) result = kIOReturnIOError;
  }
  if (gHost.poisoned) return;
  gHost.dmaEnabled = result == kIOReturnSuccess;
  native_msdc_dma = gHost.dmaEnabled ? 1 : 2;
  IOLog ("NativeMsdc: DMA comparison with PIO %s (0x%x) at 0x%llx\n", gHost.dmaEnabled ? "passed" : "failed, PIO kept",
         result, gHost.dmaPhys);
  if (!gHost.dmaEnabled) {
    gHost.dma->release ();
    gHost.dma = nullptr;
    gHost.dmaVirt = nullptr;
    gHost.dmaPhys = 0;
  }
}

bool hostBringUp (IOService *provider, bool useDma, const char **why)
{
  uint64_t phys = 0, length = 0;
  if (!controllerWindow (provider, phys, length, why)) return false;
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, 0x1000);
  gHost.map = memory ? memory->map (kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  OSSafeReleaseNULL (memory);
  gHost.lock = IOLockAlloc ();
  gHost.bounce = static_cast<uint8_t *> (IOMalloc (kTransferBytes));
  if (!gHost.map || !gHost.lock || !gHost.bounce) {
    *why = "controller map or allocation failed";
    return false;
  }
  gHost.base = static_cast<uintptr_t> (gHost.map->getVirtualAddress ());
  IOLog ("NativeMsdc: controller 0x%llx inherited CFG=0x%08x STS=0x%08x\n", phys, rd (kMsdcCfg), rd (kSdcSts));
  HostSession session;
  uint32_t response = 0;
  if (hostCommand (kCmdStatus, kStatusArgument, Data::None, &response) != kIOReturnSuccess || !cardInTransfer (response)) {
    *why = "card not in the transfer state";
    return false;
  }
  native_msdc_step = 2;
  uint8_t *extCsd = gHost.bounce;
  if (hostReadPio (kCmdSendExtCsd, 0, extCsd, 1) != kIOReturnSuccess) {
    *why = "EXT_CSD read failed";
    return false;
  }
  gHost.diskSectors = le32 (extCsd + 212);
  gHost.cache = extCsd[33] & 1;
  IOLog ("NativeMsdc: EXT_CSD rev %u, %u sectors, cache %u, partition access %u\n", extCsd[192], gHost.diskSectors,
         gHost.cache, extCsd[179] & 7);
  if (!gHost.diskSectors || (extCsd[179] & 7)) {
    *why = "EXT_CSD: no capacity or a boot/RPMB partition selected";
    return false;
  }
  if (useDma) hostEnableDma ();
  if (gHost.poisoned) {
    *why = "DMA engine did not stop";
    return false;
  }
  gHost.ready = true;
  return true;
}

const char *manifestReason (unsigned status)
{
  switch (status) {
  case kManifestNoMagic: return "no SHNGROOT manifest in userdata";
  case kManifestVersion: return "manifest version or window count";
  case kManifestHash: return "manifest SHA-256 mismatch";
  case kManifestBounds: return "manifest window bounds";
  case kManifestNames: return "manifest window names";
  case kManifestOverlap: return "manifest windows overlap";
  case kManifestAttributes: return "manifest window block size or flags";
  default: return "manifest rejected";
  }
}

// Primary GPT, the userdata partition and its manifest. Read-only.
bool hostFindLayout (const char **why)
{
  HostSession session;
  GptLayout gpt;
  if (transfer (kGptHeaderLba, 1, gHost.bounce, false) != kIOReturnSuccess) {
    *why = "GPT header read failed";
    return false;
  }
  if (const char *reason = gptCheckHeader (gHost.bounce, gHost.diskSectors, gpt)) {
    *why = reason;
    return false;
  }
  if (transfer (gpt.entriesLba, gpt.entrySectors, gHost.bounce, false) != kIOReturnSuccess) {
    *why = "GPT entries read failed";
    return false;
  }
  if (const char *reason = gptFindUserdata (gHost.bounce, gpt)) {
    *why = reason;
    return false;
  }
  gHost.gpt = gpt;
  gHost.layoutValid = true;
  native_msdc_step = 3;
  uint64_t partitionSectors = gpt.userdataLast - gpt.userdataFirst + 1;
  IOLog ("NativeMsdc: userdata sectors %llu..%llu\n", gpt.userdataFirst, gpt.userdataLast);
  if (partitionSectors < kManifestBytes / kSector ||
      transfer (gpt.userdataFirst, kManifestBytes / kSector, gHost.bounce, false) != kIOReturnSuccess) {
    *why = "userdata manifest read failed";
    return false;
  }
  uint8_t digest[32];
  SHA256_CTX context;
  SHA256_Init (&context);
  SHA256_Update (&context, gHost.bounce, kManifestHashed);
  SHA256_Final (digest, &context);
  native_msdc_manifest_status = parseManifest (gHost.bounce, digest, partitionSectors * kSector, gHost.manifest);
  gHost.manifestValid = native_msdc_manifest_status == kManifestOk;
  if (gHost.manifestValid) native_msdc_step = 4;
  return true;
}

} // namespace

// ---- Window block devices --------------------------------------------------------------------------------------------

class NativeMsdcBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeMsdcBlock)
  uint64_t firstSector = 0;   // absolute, 512-byte sectors
  uint64_t sectors = 0;
  uint32_t window = 0;        // manifest index
  char label[32] = {};
  bool writable = false;
  bool online = false;
public:
  bool configure (unsigned index, const RootWindow &entry, uint64_t absoluteSector, bool allowWrites);
  const char *windowLabel () const { return label; }
  unsigned windowIndex () const { return window; }
  bool start (IOService *provider) override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("MSDC eMMC userdata window"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = kSector; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = sectors - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override
  {
    *present = online;
    if (changed) *changed = false;
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = !writable; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override;
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = writable && gHost.cache; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeMsdcBlock, IOBlockStorageDevice)

bool NativeMsdcBlock::configure (unsigned index, const RootWindow &entry, uint64_t absoluteSector, bool allowWrites)
{
  uint64_t count = entry.size / kSector;
  if (!count || absoluteSector < gHost.gpt.userdataFirst || absoluteSector > gHost.gpt.userdataLast ||
      count > gHost.gpt.userdataLast - absoluteSector + 1)
    return false;
  window = index;
  firstSector = absoluteSector;
  sectors = count;
  writable = allowWrites && !(entry.flags & kWindowReadOnly);
  strlcpy (label, entry.name, sizeof (label));
  return true;
}

bool NativeMsdcBlock::start (IOService *provider)
{
  if (!IOBlockStorageDevice::start (provider)) return false;
  online = true;
  registerService ();
  return true;
}

IOReturn NativeMsdcBlock::doSynchronizeCache ()
{
  if (!online) return kIOReturnNotReady;
  if (!writable) return kIOReturnSuccess;
  if (gHost.poisoned) return kIOReturnNotReady;
  HostSession session;
  return hostFlush ();
}

IOReturn NativeMsdcBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                            IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= sectors || count > sectors - block ||
      buffer->getLength () < count * kSector)
    return kIOReturnBadArgument;
  IODirection direction = buffer->getDirection ();
  bool write = direction == kIODirectionOut;
  if (!write && direction != kIODirectionIn) return kIOReturnBadArgument;
  if (write && !writable) return kIOReturnNotWritable;
  if (!online) return kIOReturnNotReady;
  bool forceUnitAccess = write && attributes && (attributes->options & kIOStorageOptionForceUnitAccess);
  IOReturn result = kIOReturnSuccess;
  uint64_t done = 0;
  while (done < count && result == kIOReturnSuccess) {
    uint32_t blocks = count - done > kMaxBlocks ? kMaxBlocks : uint32_t (count - done);
    IOByteCount bytes = IOByteCount (blocks) * kSector, offset = IOByteCount (done) * kSector;
    HostSession session;
    if (write) {
      result = buffer->readBytes (offset, gHost.bounce, bytes) == bytes ?
        transfer (firstSector + block + done, blocks, gHost.bounce, true) : kIOReturnIOError;
    } else {
      result = transfer (firstSector + block + done, blocks, gHost.bounce, false);
      if (result == kIOReturnSuccess && buffer->writeBytes (offset, gHost.bounce, bytes) != bytes) result = kIOReturnIOError;
    }
    if (result == kIOReturnSuccess) done += blocks;
  }
  if (result == kIOReturnSuccess && forceUnitAccess) {
    HostSession session;
    result = hostFlush ();
  }
  completion->action (completion->target, completion->parameter, result, done * kSector);
  return kIOReturnSuccess;
}

// ---- Controller service ----------------------------------------------------------------------------------------------

class NativeMsdc : public IOService {
  OSDeclareDefaultStructors (NativeMsdc)
  bool startRoot (bool allowWrites, const char **why);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};
OSDefineMetaClassAndStructors (NativeMsdc, IOService)

namespace {

NativeMsdcBlock *gPending[kMaxWindows];
RootPlan gPlan;
NativeMsdc *gPendingProvider;
bool gVerifyWindows;

// The window's image as flashed must hash to the manifest entry. Reads the whole window.
bool windowMatchesManifest (NativeMsdcBlock *device)
{
  const RootWindow &entry = gHost.manifest.window[device->windowIndex ()];
  uint64_t first = gHost.gpt.userdataFirst + entry.offset / kSector, count = entry.size / kSector;
  SHA256_CTX context;
  SHA256_Init (&context);
  for (uint64_t done = 0; done < count;) {
    uint32_t blocks = count - done > kMaxBlocks ? kMaxBlocks : uint32_t (count - done);
    HostSession session;
    if (transfer (first + done, blocks, gHost.bounce, false) != kIOReturnSuccess) return false;
    SHA256_Update (&context, gHost.bounce, size_t (blocks) * kSector);
    done += blocks;
  }
  uint8_t digest[32];
  SHA256_Final (digest, &context);
  return !memcmp (digest, entry.sha256, 32);
}

// An APFS volume whose provider chain reaches this window: the window's container has been opened.
bool volumeBelow (IOService *window)
{
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  bool found = false;
  for (unsigned count = 0; iterator && !found && count < 256; ++count) {
    IORegistryEntry *media = OSDynamicCast (IORegistryEntry, iterator->getNextObject ());
    if (!media) break;
    if (strcmp (media->getMetaClass ()->getClassName (), "AppleAPFSVolume")) continue;
    IORegistryIterator *parents =
      IORegistryIterator::iterateOver (media, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
    for (unsigned steps = 0; parents && steps < 256; ++steps) {
      IORegistryEntry *entry = parents->getNextObject ();
      if (!entry) break;
      if (entry == window) {
        found = true;
        break;
      }
    }
    OSSafeReleaseNULL (parents);
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
  return found;
}

void discardPending (unsigned index)
{
  if (NativeMsdcBlock *device = gPending[index]) {
    device->detach (gPendingProvider);
    device->release ();
    gPending[index] = nullptr;
  }
}

// Ordered publication: each window before OS waits (up to 30 s) until its APFS volume appears, so SystemOS is
// resolvable when the root mounts. A window that fails its hash or its start is not published; the rest continue.
void publishThread (void *, wait_result_t)
{
  for (unsigned position = 0; position < gPlan.count; ++position) {
    unsigned index = gPlan.order[position];
    NativeMsdcBlock *device = gPending[index];
    if (!device) continue;
    if (gVerifyWindows && !windowMatchesManifest (device)) {
      IOLog ("NativeMsdc: %s does not match its manifest SHA-256; not published\n", device->windowLabel ());
      native_msdc_failure = "window SHA-256 mismatch";
      discardPending (index);
      continue;
    }
    if (!device->start (gPendingProvider)) {
      IOLog ("NativeMsdc: %s did not start; not published\n", device->windowLabel ());
      native_msdc_failure = "window start failed";
      discardPending (index);
      continue;
    }
    gPending[index] = nullptr;          // owned by the registry from here on
    native_msdc_blocks_published = native_msdc_blocks_published + 1;
    IOLog ("NativeMsdc: published %s\n", device->windowLabel ());
    if (position + 1 == gPlan.count) {
      device->release ();
      break;
    }
    bool ready = false;
    for (unsigned tick = 0; tick < 300 && !(ready = volumeBelow (device)); ++tick) IOSleep (100);
    IOLog ("NativeMsdc: %s volume %s\n", device->windowLabel (), ready ? "ready" : "not seen after 30 s");
    device->release ();
  }
  native_msdc_step = 6;
  gPendingProvider->release ();
}

} // namespace

IOService *NativeMsdc::probe (IOService *provider, SInt32 *score)
{
  if (!IOService::probe (provider, score) || !listHas (provider->getProperty ("compatible"), "mediatek,mt8167-mmc"))
    return nullptr;
  if (score) *score = 1000;
  return this;
}

bool NativeMsdc::start (IOService *provider)
{
  unsigned root = bootArgument ("native-msdc-root", 2, 0);
  unsigned persist = bootArgument ("native-msdc-persist", 2, 0);
  unsigned dma = bootArgument ("native-msdc-dma", 1, 0);
  gVerifyWindows = bootArgument ("native-msdc-verify", 1, 0) == 1;
  if (!root && !persist) return false;   // matched only; the controller is not touched
  if (gHost.base) {
    IOLog ("NativeMsdc: one controller only; %s ignored\n", provider->getName ());
    return false;
  }
  native_msdc_step = 1;
  if (!IOService::start (provider)) return false;
  const char *why = "";
  if (!hostBringUp (provider, dma == 1, &why) || !hostFindLayout (&why)) {
    fail (why);
    IOService::stop (provider);
    return false;
  }
  if (!gHost.manifestValid) fail (manifestReason (native_msdc_manifest_status));
  else if (root && !startRoot (root == 2, &why)) fail (why);
  return true;
}

bool NativeMsdc::startRoot (bool allowWrites, const char **why)
{
  if (!buildRootPlan (gHost.manifest, gPlan)) {
    *why = "manifest has no OS window";
    return false;
  }
  gPendingProvider = this;
  for (unsigned index = 0; index < gHost.manifest.count; ++index) {
    const RootWindow &entry = gHost.manifest.window[index];
    NativeMsdcBlock *device = OSTypeAlloc (NativeMsdcBlock);
    gPending[index] = device;
    if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) ||
        !device->configure (index, entry, gHost.gpt.userdataFirst + entry.offset / kSector, allowWrites)) {
      if (device) device->release ();
      gPending[index] = nullptr;
      while (index) discardPending (--index);
      *why = "window preparation failed";
      return false;
    }
    if (!device->attach (this)) {
      device->release ();
      gPending[index] = nullptr;
      while (index) discardPending (--index);
      *why = "window attach failed";
      return false;
    }
  }
  retain ();
  thread_t thread = nullptr;
  if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
    release ();
    for (unsigned index = gHost.manifest.count; index;) discardPending (--index);
    *why = "publisher thread";
    return false;
  }
  thread_deallocate (thread);
  native_msdc_step = 5;
  return true;
}

void NativeMsdc::stop (IOService *provider)
{
  // Published windows may still have clients; the host stays as it is.
  IOService::stop (provider);
}

// ---- Persistent store window (NativeRamBlock's overlay, see PersistOverlay.h) --------------------------------------
// 4096-byte blocks, relative to the window. Mode 1 only reads; mode 2 also writes. Writes return when the blocks are
// on the medium (written, then the card cache flushed), so the overlay needs no separate flush.
namespace {
uint64_t gStoreFirstSector, gStoreBlocks;

unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) cached = int (bootArgument ("native-msdc-persist", 2, 0));
  return unsigned (cached);
}

uint64_t reserveBlocks (void)
{
  return uint64_t (bootArgument ("native-msdc-reserve", 16384, 1024)) * (1024 * 1024 / kStoreBlock);
}

bool storeTransfer (uint64_t relative, uint32_t blocks, uint8_t *data, bool write)
{
  if (!gStoreBlocks || !data || !blocks || relative >= gStoreBlocks || blocks > gStoreBlocks - relative) return false;
  constexpr uint32_t perTransfer = uint32_t (kMaxBlocks / kSectorsPerStoreBlock);
  while (blocks) {
    uint32_t chunk = blocks > perTransfer ? perTransfer : blocks;
    HostSession session;
    if (transfer (gStoreFirstSector + relative * kSectorsPerStoreBlock, uint32_t (chunk * kSectorsPerStoreBlock), data,
                  write) != kIOReturnSuccess)
      return false;
    data += chunk * kStoreBlock;
    relative += chunk;
    blocks -= chunk;
  }
  return true;
}
} // namespace

extern "C" unsigned native_msdc_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 512-byte sector of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_msdc_store_open (uint64_t blocks, uint64_t *firstSector, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gHost.ready || !gHost.manifestValid || gHost.poisoned) reason = "no root layout";
  else {
    HostSession session;
    uint64_t first = 0;
    if (gStoreBlocks) reason = "already open";
    else if (const char *refused = storeWindow (gHost.gpt.userdataLast - gHost.gpt.userdataFirst + 1,
                                                gHost.manifest.imageBytes, reserveBlocks (), blocks, first))
      reason = refused;
    else {
      gStoreFirstSector = gHost.gpt.userdataFirst + first * kSectorsPerStoreBlock;
      gStoreBlocks = blocks;
      if (firstSector) *firstSector = gStoreFirstSector;
      ok = 1;
    }
  }
  if (why) *why = reason;
  if (ok) IOLog ("NativeMsdc: store at sector %llu, %llu blocks, mode %u\n", gStoreFirstSector, gStoreBlocks, storeMode ());
  else IOLog ("NativeMsdc: store refused: %s\n", reason);
  return ok;
}

extern "C" int native_msdc_store_read (uint64_t relative, uint32_t blocks, void *destination)
{
  return storeTransfer (relative, blocks, static_cast<uint8_t *> (destination), false);
}

extern "C" int native_msdc_store_write (uint64_t relative, uint32_t blocks, const void *source)
{
  if (storeMode () != 2) return 0;
  // transfer() only reads from the buffer on a write.
  if (!storeTransfer (relative, blocks, static_cast<uint8_t *> (const_cast<void *> (source)), true)) return 0;
  HostSession session;
  return hostFlush () == kIOReturnSuccess;
}

extern "C" int native_msdc_store_sync (void)
{
  if (storeMode () != 2 || !gStoreBlocks || gHost.poisoned) return 0;
  HostSession session;
  return hostFlush () == kIOReturnSuccess;
}

// Re-read the GPT header and entry array and compare their CRCs with those recorded at start. 0 = unchanged.
extern "C" unsigned native_msdc_store_gpt_check (void)
{
  if (!gHost.ready || !gHost.layoutValid || gHost.poisoned) return 1;
  HostSession session;
  if (transfer (kGptHeaderLba, 1, gHost.bounce, false) != kIOReturnSuccess) return 2;
  GptLayout now;
  if (gptCheckHeader (gHost.bounce, gHost.diskSectors, now) || now.headerCrc != gHost.gpt.headerCrc) return 3;
  if (transfer (gHost.gpt.entriesLba, gHost.gpt.entrySectors, gHost.bounce, false) != kIOReturnSuccess) return 4;
  return gptCrc32 (gHost.bounce, size_t (gHost.gpt.entryCount) * kGptEntryBytes) == gHost.gpt.entriesCrc ? 0 : 5;
}

#if defined(NATIVE_MSDC_UFS_STORE_ABI) && NATIVE_MSDC_UFS_STORE_ABI
// NativeRamBlock imports the store under the native_ufs_store_* names (weak). A board without NativeUfs selects this
// to give it the eMMC store instead; linking both drivers with it fails on the duplicate symbols.
extern "C" unsigned native_ufs_store_mode (void) { return native_msdc_store_mode (); }
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *first, const char **why)
{
  return native_msdc_store_open (blocks, first, why);
}
extern "C" int native_ufs_store_read (uint64_t relative, uint32_t blocks, void *destination)
{
  return native_msdc_store_read (relative, blocks, destination);
}
extern "C" int native_ufs_store_write (uint64_t relative, uint32_t blocks, const void *source)
{
  return native_msdc_store_write (relative, blocks, source);
}
extern "C" int native_ufs_store_sync (void) { return native_msdc_store_sync (); }
extern "C" unsigned native_ufs_store_gpt_check (void) { return native_msdc_store_gpt_check (); }
#endif
