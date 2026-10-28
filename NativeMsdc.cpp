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
//
// NATIVE_MSDC_BLOCK_DEVICES (default 1) builds the window block devices. They subclass the original
// IOBlockStorageDevice of the prelinked IOStorageFamily, so the kernel link needs the storage bindings
// (build-native-virtio-bindings.py). With 0 the driver serves the persistent store only and links on its own.
//
// NATIVE_VERIFIED_BLOCK=1 (default off, needs the window block devices) serves the windows only through the
// owner-signed VerifiedRoot policies stored after them in userdata (layout in NativeMsdc.h, "Signed root policy
// region"). Before publishing, the publisher thread reads the whole region into read-only kernel memory; each window is
// published only if exactly one policy has its size and vr_open accepts it under verified_root_key. Every read is then
// served from whole 4096-byte blocks that passed vr_verify_block, writes are refused, and native_ufs_block_authenticates
// answers the root authorization of NativeVirtioBlock for these windows. Independent of native-msdc-verify.
#ifndef NATIVE_MSDC_BLOCK_DEVICES
#define NATIVE_MSDC_BLOCK_DEVICES 1
#endif
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
#define NATIVE_MSDC_VERIFIED 1
#if !NATIVE_MSDC_BLOCK_DEVICES
#error "NATIVE_VERIFIED_BLOCK=1 needs the window block devices (NATIVE_MSDC_BLOCK_DEVICES=1)"
#endif
#else
#define NATIVE_MSDC_VERIFIED 0
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#if NATIVE_MSDC_BLOCK_DEVICES
#include <IOKit/storage/IOBlockStorageDevice.h>
#endif
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#ifndef NATIVE_MSDC_IRQ_WAIT
#define NATIVE_MSDC_IRQ_WAIT 0
#endif
#ifndef NATIVE_MSDC_WORKER_EXPERIMENT
#define NATIVE_MSDC_WORKER_EXPERIMENT 0
#endif
#if NATIVE_MSDC_WORKER_EXPERIMENT && !(NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_VERIFIED)
#error "The experimental read worker requires IRQ wait, block devices and verified root"
#endif
#if NATIVE_MSDC_IRQ_WAIT
#include <machine/machine_routines.h>
extern "C" int get_preemption_level (void);
extern "C" bool armcpu_spi_boot_ready (void);
extern "C" IOReturn armcpu_spi_attach_sysirq (uint32_t, volatile uint32_t *, void (*) (void *), void *,
                                               uint32_t *, uint64_t *, bool *);
extern "C" bool armcpu_spi_mask_owned (uint32_t, void (*) (void *), void *);
#endif
#include <pexpert/pexpert.h>
#if NATIVE_MSDC_VERIFIED
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include "verified_root.h"
extern "C" {
extern const struct vr_crypto vr_xnu_crypto;     // kits/VerifiedRoot/adapters/xnu_crypto.c
extern const uint8_t verified_root_key[];         // the owner key, VR_KEY_BYTES, linked separately
}
#endif
#include "NativeMsdc.h"

using namespace NativeMsdcHw;

extern "C" {
// Boot diagnostics: 1 started, 2 card ready, 3 userdata found, 4 manifest valid, 5 windows scheduled, 6 windows published.
volatile uint32_t native_msdc_step = 0;
const char *volatile native_msdc_failure = nullptr;
volatile uint32_t native_msdc_manifest_status = 0xffffffff;
volatile uint32_t native_msdc_dma = 0;                      // 0 PIO, 1 DMA verified, 2 DMA rejected, 3 quarantined
volatile uint32_t native_msdc_blocks_published = 0;
#if NATIVE_MSDC_BLOCK_DEVICES
volatile uint32_t native_msdc_publisher_terminate_failed = 0;
#endif
#if NATIVE_MSDC_IRQ_WAIT
// IRQs captured, wait attempts, delivery fallback, elapsed wait nanoseconds.
volatile uint64_t native_msdc_irq_stats[4] = {};
volatile uint64_t native_msdc_irq_mask_failures = 0;
// Queued reads, completed I/O requests, saturation/thread-start fallbacks, reserved.
volatile uint64_t native_msdc_worker_stats[4] = {};
#endif
volatile uint64_t native_msdc_reads = 0, native_msdc_writes = 0, native_msdc_flushes = 0, native_msdc_failures = 0;
// Optional verified-root snapshot reuse: hits, misses, authenticated bytes reused.
volatile uint64_t native_msdc_verified_cache_stats[3] = {};
// read calls/requested bytes/ns, verify calls/bytes/ns; no hot-path logging.
volatile uint64_t native_msdc_verified_timing_stats[6] = {};
void native_root_timing_snapshot (uint64_t out[6])
{
  for (unsigned i = 0; i < 6; ++i)
    out[i] = __atomic_load_n (&native_msdc_verified_timing_stats[i], __ATOMIC_RELAXED);
}


// Signed root policy (NATIVE_VERIFIED_BLOCK): header status (PolicyStatus, 0xffffffff not read), windows that passed
// vr_open, verified 4096-byte blocks served and integrity failures.
volatile uint32_t native_msdc_policy_status = 0xffffffff;
volatile uint32_t native_msdc_verified_windows = 0;
volatile uint64_t native_msdc_verified_blocks = 0;
volatile uint32_t native_msdc_integrity_failures = 0;
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
#if NATIVE_MSDC_IRQ_WAIT
  IOSimpleLock *irqLock;
  IOMemoryMap *sysirqMap;
  bool irqReady, irqArmed, irqDelivered;
  uint32_t irqStatus, irqSpi, irqSpurious;
  volatile uint32_t irqFault;
#endif
  GptLayout gpt;
  bool layoutValid;
  RootManifest manifest;
  bool manifestValid;
};
MsdcHost gHost;
#if NATIVE_MSDC_BLOCK_DEVICES
bool gPublisherStopped = false;
// 0 waiting, 1 stop requested, 2 publisher could not be started.
uint32_t gPublisherCleanup = 0;
#endif
#if NATIVE_MSDC_IRQ_WAIT
bool gActivationStopped = false;
IOLock *gActivationLock = nullptr;
#endif
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES
// Only explicit publication-thread reads opt in; storage callbacks and store
// ABI callers retain polling until their complete lock/caller contracts are proven.
thread_t gPublisherThread = nullptr;
#if NATIVE_MSDC_WORKER_EXPERIMENT
thread_t gIoWorkerThread = nullptr;
#endif
#endif
bool gModeTelemetryEnabled; // exact native-msdc-mode=1, sampled once before bring-up

inline uint32_t rd (unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (gHost.base + offset);
}

inline void wr (unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (gHost.base + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

uint64_t deadlineAfter (uint64_t nanoseconds);

#if NATIVE_MSDC_IRQ_WAIT
// HostSession's mutex serializes requests; the primary IRQ handler uses only
// this independent spin lock, never the mutex. Events are latched before W1C.
constexpr uint32_t kDmaEvents = kIntXferDone | kIntDataErrors | kIntDmaErrors;
void dmaIrq (void *)
{
  IOInterruptState state = IOSimpleLockLockDisableInterrupt (gHost.irqLock);
  uint32_t events = gHost.irqArmed ? rd (kMsdcIntr) & rd (kMsdcIntEn) & kDmaEvents : 0;
  bool wake = events != 0;
  if (events) {
    gHost.irqSpurious = 0;
    gHost.irqStatus |= events;
    gHost.irqDelivered = true;
    __atomic_fetch_add (&native_msdc_irq_stats[0], 1, __ATOMIC_RELAXED);
    wr (kMsdcIntEn, 0);
    wr (kMsdcIntr, events);
  } else if (++gHost.irqSpurious >= 8) {
    // An asserted line with no enabled DMA event must not storm. Mask only
    // our registered GIC SPI, and force an active waiter back to polling.
    bool masked = armcpu_spi_mask_owned (gHost.irqSpi + 32, dmaIrq, nullptr);
    __atomic_store_n (&gHost.irqFault, 1, __ATOMIC_RELEASE);
    wr (kMsdcIntEn, 0);
    wake = true;
    if (!masked) {
      __atomic_fetch_add (&native_msdc_irq_mask_failures, 1, __ATOMIC_RELAXED);
      // If neither layer can be silenced, stop rather than loop in IRQ forever.
      if (rd (kMsdcIntEn) || gHost.irqSpurious >= 16)
        panic ("NativeMsdc: unmaskable interrupt storm");
    }
    if (gHost.irqSpurious >= 32)
      panic ("NativeMsdc: interrupt continues after mask");
  }
  IOSimpleLockUnlockEnableInterrupt (gHost.irqLock, state);
  if (wake) thread_wakeup (reinterpret_cast<event_t> (&gHost.irqStatus));
}

// INTEN remains zero during commands and after this function. Holding the
// spin lock across the status check and assert_wait prevents a lost wakeup.
IOReturn waitDmaIrq (uint64_t end)
{
  // Bound an undeliverable IRQ to 10 ms, then finish this transfer by polling.
  __atomic_fetch_add (&native_msdc_irq_stats[1], 1, __ATOMIC_RELAXED);
  uint64_t waitStart = mach_absolute_time ();
  uint64_t probeEnd = deadlineAfter (10000000ULL);
  if (probeEnd > end) probeEnd = end;
  IOInterruptState state = IOSimpleLockLockDisableInterrupt (gHost.irqLock);
  bool slept = false;
  gHost.irqStatus = 0;
  gHost.irqDelivered = false;
  gHost.irqArmed = true;
  wr (kMsdcIntEn, kDmaEvents);
  for (;;) {
    uint32_t events = rd (kMsdcIntr) & kDmaEvents;
    if (events) { gHost.irqStatus |= events; wr (kMsdcIntr, events); }
    uint32_t observed = gHost.irqStatus;
    if (observed || __atomic_load_n (&gHost.irqFault, __ATOMIC_ACQUIRE) || mach_absolute_time () >= probeEnd) {
      wr (kMsdcIntEn, 0);
      // Capture an event arriving between the first status read and masking.
      events = rd (kMsdcIntr) & kDmaEvents;
      if (events) { observed |= events; wr (kMsdcIntr, events); }
      gHost.irqArmed = false;
      bool noDelivery = slept && mach_absolute_time () >= probeEnd && !gHost.irqDelivered;
      if (noDelivery) {
        __atomic_store_n (&gHost.irqReady, false, __ATOMIC_RELEASE);
        __atomic_fetch_add (&native_msdc_irq_stats[2], 1, __ATOMIC_RELAXED);
      }
      IOSimpleLockUnlockEnableInterrupt (gHost.irqLock, state);
      uint64_t elapsedNs = 0;
      absolutetime_to_nanoseconds (mach_absolute_time () - waitStart, &elapsedNs);
      __atomic_fetch_add (&native_msdc_irq_stats[3], elapsedNs, __ATOMIC_RELAXED);
      if (observed & (kIntDataErrors | kIntDmaErrors)) return kIOReturnIOError;
      if (observed & kIntXferDone) return kIOReturnSuccess;
      // Delivery was not proven; do not pay this delay on subsequent reads.
      __atomic_store_n (&gHost.irqReady, false, __ATOMIC_RELEASE);
      return kIOReturnNotReady;
    }
    wait_result_t pending = assert_wait_deadline (reinterpret_cast<event_t> (&gHost.irqStatus), THREAD_UNINT, probeEnd);
    IOSimpleLockUnlockEnableInterrupt (gHost.irqLock, state);
    if (pending == THREAD_WAITING) { slept = true; (void) thread_block (THREAD_CONTINUE_NULL); }
    state = IOSimpleLockLockDisableInterrupt (gHost.irqLock);
  }
}
#endif

#if NATIVE_MSDC_VERIFIED
bool gRootTimingEnabled; // configured once before window publication
uint64_t rootTimingNow ()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (mach_absolute_time (), &ns);
  return ns;
}
#endif

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
IOReturn hostReadDma (uint32_t argument, uint8_t *buffer, uint32_t blocks, bool publisherRead = false)
{
#if !(NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES)
  (void) publisherRead;
#endif
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
#if NATIVE_MSDC_IRQ_WAIT
#if NATIVE_MSDC_BLOCK_DEVICES
    thread_t self = current_thread ();
    const bool publisherContext = publisherRead &&
      (self == __atomic_load_n (&gPublisherThread, __ATOMIC_ACQUIRE)
#if NATIVE_MSDC_WORKER_EXPERIMENT
       || self == __atomic_load_n (&gIoWorkerThread, __ATOMIC_ACQUIRE)
#endif
      );
#else
    const bool publisherContext = false;
#endif
    if (publisherContext && __atomic_load_n (&gHost.irqReady, __ATOMIC_ACQUIRE) && !__atomic_load_n (&gHost.irqFault, __ATOMIC_ACQUIRE) &&
        ml_get_interrupts_enabled () && get_preemption_level () == 0) {
      result = waitDmaIrq (end);
    } else result = kIOReturnNotReady;
    if (result == kIOReturnNotReady) {
      result = kIOReturnSuccess;
#endif
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
#if NATIVE_MSDC_IRQ_WAIT
    }
#endif
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
  uint32_t inheritedInterrupts () const { return interruptEnable; }
  HostSession (const HostSession &) = delete;
  HostSession &operator= (const HostSession &) = delete;
};

// One transfer of at most kMaxBlocks sectors anywhere on the user area; callers bound it to their own region.
IOReturn transfer (uint64_t sector, uint32_t blocks, uint8_t *buffer, bool write, bool publisherRead = false)
{
  if (gHost.poisoned) return kIOReturnNotReady;
  if (!blocks || blocks > kMaxBlocks || sector >= gHost.diskSectors || blocks > gHost.diskSectors - sector)
    return kIOReturnBadArgument;
  IOReturn result;
  if (write) result = hostWrite (uint32_t (sector), buffer, blocks);
  else if (gHost.dmaEnabled) result = hostReadDma (uint32_t (sector), buffer, blocks, publisherRead);
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

// Diagnostic reads only: no card command, clock/pad write, new transfer, reset,
// retune or mode transition. EXT_CSD is the already successful 512-byte CMD8.
// TOPCKGEN address/mux [13:11] are documented by the MT8167 Linux/EDK2 sources.
// A selector alone does not prove the live PLL/source frequency.
// EXT_CSD[183] is not readable: retain raw bytes only, never infer card
// width/DDR or agreement with the host. HS_TIMING[185] is readable.
void hostLogMode (const uint8_t *extCsd, uint32_t inheritedCfg)
{
  if (!gModeTelemetryEnabled) return;
  uint32_t cfg = rd (kMsdcCfg), io = rd (kMsdcIoCon), sdc = rd (kSdcCfg);
  uint32_t pad0 = rd (kPadTune0), pad1 = rd (kPadTune1);
  BusMode mode = decodeBusMode (cfg, sdc, extCsd[185]);
  uint32_t muxReg = 0;
  bool muxValid = false;
  IODeviceMemory *clockMemory = IODeviceMemory::withRange (0x10000000ULL, 0x1000);
  IOMemoryMap *clockMap = clockMemory ? clockMemory->map (kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (clockMap && clockMap->getLength () >= sizeof (uint32_t) && clockMap->getVirtualAddress ()) {
    muxReg = *reinterpret_cast<volatile uint32_t *> (uintptr_t (clockMap->getVirtualAddress ()));
    muxValid = true;
  }
  OSSafeReleaseNULL (clockMap);
  OSSafeReleaseNULL (clockMemory);
  // Keep every newline-terminated record below the 255-byte console buffer.
  IOLog ("NativeMsdc: mode raw inherited=%08x cfg=%08x iocon=%08x sdc=%08x pad0=%08x pad1=%08x\n",
         inheritedCfg, cfg, io, sdc, pad0, pad1);
  IOLog ("NativeMsdc: mode host ckmod=%u ckdiv=%u stable=%u bits=%u ddr=%u hs400clk=%u\n",
         mode.clockMode, mode.clockDivider, unsigned (mode.clockStable), mode.hostBits,
         unsigned (mode.hostDdr), unsigned ((cfg >> 22) & 1u));
  IOLog ("NativeMsdc: mode card rev=%u bus_width_raw=%02x bus_width=not_readable hs_timing=%02x card_type=%02x driver_strength=%02x timing=%u\n",
         unsigned (extCsd[192]), unsigned (extCsd[183]), unsigned (extCsd[185]), unsigned (extCsd[196]),
         unsigned (extCsd[197]), mode.cardTiming);
  IOLog ("NativeMsdc: mode source mux_valid=%u mux_reg=%08x msdc0_mux=%u source_hz=unmeasured\n",
         unsigned (muxValid), muxReg, muxValid ? (muxReg >> 11) & 7u : 255u);
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

#if NATIVE_MSDC_IRQ_WAIT
// Board-specific wiring is passed as ADT metadata. Refuse unexpected shapes
// or an inherited enabled source; startup and the DMA self-test remain polled.
void hostEnableIrq (IOService *provider, HostSession &session)
{
  OSData *spiData = OSDynamicCast (OSData, provider->getProperty ("msdc-irq-spi"));
  OSData *baseData = OSDynamicCast (OSData, provider->getProperty ("msdc-sysirq-base"));
  OSData *lowData = OSDynamicCast (OSData, provider->getProperty ("msdc-irq-low"));
  if (!spiData || spiData->getLength () != 4 || !baseData || baseData->getLength () != 8 ||
      !lowData || lowData->getLength () != 4 || !gHost.dmaEnabled) {
    IOLog ("NativeMsdc: IRQ unavailable metadata spi=%u base=%u low=%u dma=%u\n",
           spiData ? spiData->getLength () : 0, baseData ? baseData->getLength () : 0,
           lowData ? lowData->getLength () : 0, unsigned (gHost.dmaEnabled));
    return;
  }
  uint32_t spi = 0, low = 0; uint64_t base = 0;
  memcpy (&spi, spiData->getBytesNoCopy (), 4);
  memcpy (&base, baseData->getBytesNoCopy (), 8);
  memcpy (&low, lowData->getBytesNoCopy (), 4);
  if (spi >= 256 || low != 1 || !base || (base & 3) || (base & 4095) + 32 > 4096) {
    IOLog ("NativeMsdc: IRQ unavailable invalid wiring spi=%u low=%u base=0x%llx\n", spi, low,
           static_cast<unsigned long long> (base));
    return;
  }
  if (session.inheritedInterrupts ()) {
    IOLog ("NativeMsdc: IRQ unavailable inherited enables=0x%x\n", session.inheritedInterrupts ());
    return;
  }
  gHost.irqLock = IOSimpleLockAlloc ();
  IODeviceMemory *memory = IODeviceMemory::withRange (base & ~uint64_t (4095), 4096);
  gHost.sysirqMap = memory ? memory->map (kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  OSSafeReleaseNULL (memory);
  if (!gHost.irqLock || !gHost.sysirqMap) {
    IOLog ("NativeMsdc: IRQ unavailable allocation lock=%u mapping=%u\n", unsigned (gHost.irqLock != nullptr),
           unsigned (gHost.sysirqMap != nullptr));
    if (gHost.irqLock) IOSimpleLockFree (gHost.irqLock);
    gHost.irqLock = nullptr;
    OSSafeReleaseNULL (gHost.sysirqMap);
    return;
  }
  auto *polarity = reinterpret_cast<volatile uint32_t *> (gHost.sysirqMap->getVirtualAddress () +
                                                           (base & 4095) + (spi / 32) * 4);
  gHost.irqSpi = spi;
  uint32_t readback = 0; uint64_t target = 0; bool installed = false;
  // All potentially blocking allocation and mapping happened before this
  // gate. Stop never waits for the host mutex or a worker allocation here.
  IOLockLock (gActivationLock);
  if (__atomic_load_n (&gActivationStopped, __ATOMIC_ACQUIRE)) {
    IOLockUnlock (gActivationLock);
    IOSimpleLockFree (gHost.irqLock); gHost.irqLock = nullptr;
    OSSafeReleaseNULL (gHost.sysirqMap);
    return;
  }
  IOReturn status = armcpu_spi_attach_sysirq (spi, polarity, dmaIrq, nullptr, &readback, &target, &installed);
  __atomic_store_n (&gHost.irqReady, status == kIOReturnSuccess && !__atomic_load_n (&gHost.irqFault, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
  IOLockUnlock (gActivationLock);
  if (status != kIOReturnSuccess && !installed) {
    IOLog ("NativeMsdc: IRQ unavailable attach status=0x%x spi=%u gic=0x%x target=0x%llx\n",
           unsigned (status), spi, readback, static_cast<unsigned long long> (target));
    IOSimpleLockFree (gHost.irqLock); gHost.irqLock = nullptr;
    OSSafeReleaseNULL (gHost.sysirqMap);
    return;
  }
  // A failed enable/readback may still have published a handler. ArmCpu
  // attempts to mask the source; keep the handler state alive even if the
  // readback says that masking failed.
  IOLog ("NativeMsdc: IRQ wait %s spi=%u gic=0x%x target=0x%llx\n",
         __atomic_load_n (&gHost.irqReady, __ATOMIC_ACQUIRE) ? "ready" : "unavailable", spi, readback, target);
}
#endif

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
  const uint32_t inheritedCfg = rd (kMsdcCfg);
  IOLog ("NativeMsdc: controller 0x%llx inherited CFG=0x%08x STS=0x%08x\n", phys, inheritedCfg, rd (kSdcSts));
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
  hostLogMode (extCsd, inheritedCfg);
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

#if NATIVE_MSDC_BLOCK_DEVICES
// ---- Window block devices --------------------------------------------------------------------------------------------

#if NATIVE_MSDC_VERIFIED
// ---- Signed root policy region -----------------------------------------------------------------------------------------
namespace {

// The region as read from userdata, then made read-only, followed by a read-only copy of the owner key on its own page.
// vr_open requires both to stay immutable for as long as the views opened on them are used: the windows never free it.
struct PolicyStore {
  vm_offset_t storage;
  vm_size_t storageBytes;
  const uint8_t *key;
  PolicyHeader header;
};
PolicyStore gPolicy;

void policyDigest (const uint8_t *header, uint8_t digest[32])
{
  static const uint8_t zero[32] = {};
  SHA256_CTX context;
  SHA256_Init (&context);
  SHA256_Update (&context, header, kPolicyHashed);
  SHA256_Update (&context, zero, sizeof (zero));
  SHA256_Final (digest, &context);
}

const char *policyReason (unsigned status)
{
  switch (status) {
  case kPolicyNoMagic: return "no A04RPOL1 policy region after the root windows";
  case kPolicyVersion: return "policy version or entry count";
  case kPolicyHash: return "policy header SHA-256 mismatch";
  case kPolicySize: return "policy region size";
  case kPolicyEntryBounds: return "policy entry bounds";
  case kPolicyEntryOverlap: return "policy entries overlap";
  default: return "policy region rejected";
  }
}

void freePolicy (void)
{
  if (gPolicy.storage) kmem_free (kernel_map, gPolicy.storage, gPolicy.storageBytes);
  gPolicy = {};
}

// Reads the whole region into kernel memory, checks its header again from that copy and makes it read-only.
// Returns nullptr or the reason the region is unusable.
const char *loadPolicy (void)
{
  uint64_t partitionBytes = (gHost.gpt.userdataLast - gHost.gpt.userdataFirst + 1) * kSector;
  uint64_t start = policyRegionOffset (gHost.manifest);
  if (start >= partitionBytes || partitionBytes - start < kPolicyHeaderBytes) return "policy region outside userdata";
  uint64_t available = partitionBytes - start, firstSector = gHost.gpt.userdataFirst + start / kSector;
  PolicyHeader header;
  unsigned status;
  uint8_t digest[32];
  {
    HostSession session;
    if (transfer (firstSector, kPolicyHeaderBytes / kSector, gHost.bounce, false, true) != kIOReturnSuccess)
      return "policy header read failed";
    policyDigest (gHost.bounce, digest);
    status = parsePolicyHeader (gHost.bounce, digest, available, header);
  }
  native_msdc_policy_status = status;
  if (status != kPolicyOk) return policyReason (status);
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (header.totalBytes));
  vm_size_t keyBytes = round_page (static_cast<vm_size_t> (VR_KEY_BYTES));
  vm_offset_t storage = 0;
  if (kmem_alloc (kernel_map, &storage, tableBytes + keyBytes, static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA),
                  VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS)
    return "policy allocation";
  gPolicy.storage = storage;
  gPolicy.storageBytes = tableBytes + keyBytes;
  uint8_t *region = reinterpret_cast<uint8_t *> (storage);
  IOLog ("NativeMsdc: reading the signed root policy, %llu bytes at userdata +%llu\n", header.totalBytes, start);
  uint64_t began = mach_absolute_time ();
  uint64_t sectors = (header.totalBytes + kSector - 1) / kSector;
  for (uint64_t done = 0; done < sectors;) {
#if NATIVE_MSDC_BLOCK_DEVICES
    if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE)) {
      freePolicy ();
      return "policy read cancelled";
    }
#endif
    uint32_t blocks = sectors - done > kMaxBlocks ? kMaxBlocks : uint32_t (sectors - done);
    HostSession session;
    if (transfer (firstSector + done, blocks, region + done * kSector, false, true) != kIOReturnSuccess) {
      freePolicy ();
      return "policy region read failed";
    }
    done += blocks;
  }
  uint64_t elapsed = 0;
  absolutetime_to_nanoseconds (mach_absolute_time () - began, &elapsed);
  // The header is taken from the copy that the envelopes are opened from, and must describe the same region.
  policyDigest (region, digest);
  status = parsePolicyHeader (region, digest, available, gPolicy.header);
  native_msdc_policy_status = status;
  if (status != kPolicyOk || gPolicy.header.totalBytes != header.totalBytes) {
    freePolicy ();
    return status != kPolicyOk ? policyReason (status) : "policy region changed while it was read";
  }
  uint8_t *key = region + tableBytes;
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  if (vm_map_protect (kernel_map, storage, storage + tableBytes + keyBytes, VM_PROT_READ, TRUE) != KERN_SUCCESS) {
    freePolicy ();
    return "policy read-only protection";
  }
  gPolicy.key = key;
  IOLog ("NativeMsdc: signed root policy read in %llu ms, %u entries\n", elapsed / 1000000ULL, gPolicy.header.count);
  return nullptr;
}

const char *vrReason (int status)
{
  switch (status) {
  case VR_FORMAT: return "policy envelope format or generation";
  case VR_KEY: return "policy envelope is for another owner key";
  case VR_SIGNATURE: return "policy owner signature invalid";
  case VR_HASH_TABLE: return "policy block hash table does not match its signed digest";
  default: return "policy envelope rejected";
  }
}

} // namespace
#endif // NATIVE_MSDC_VERIFIED

#if NATIVE_MSDC_VERIFIED
// Reconfiguration shares the existing per-window read lock, including failures.
class VerifiedWindowGuard {
  IOLock *mutex;
public:
  explicit VerifiedWindowGuard (IOLock *lock) : mutex (lock) { if (mutex) IOLockLock (mutex); }
  ~VerifiedWindowGuard () { if (mutex) IOLockUnlock (mutex); }
};
#endif

class NativeMsdcBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeMsdcBlock)
  uint64_t firstSector = 0;   // absolute, 512-byte sectors
  uint64_t sectors = 0;
  uint32_t window = 0;        // manifest index
  char label[32] = {};
  bool writable = false;
  bool online = false;
#if NATIVE_MSDC_VERIFIED
  bool verifiedActive = false;
  bool verifiedCacheEnabled = false;
  bool manifestReadOnly = false;
  uint64_t verifiedConfiguration = 0;
  VerifiedReadCache verifiedCache;
  struct vr_view verified = {};
  const uint8_t *envelope = nullptr;    // the selected VROOT01 envelope in the read-only policy copy
  uint8_t *verifiedBounce = nullptr;    // kTransferBytes, under verifiedLock from read through copy
  IOLock *verifiedLock = nullptr;
  IOReturn readVerified (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count, IOStorageCompletion *completion,
                         uint64_t configuration);
#endif
public:
  bool configure (unsigned index, const RootWindow &entry, uint64_t absoluteSector, bool allowWrites);
#if NATIVE_MSDC_VERIFIED
  const char *configureVerification (int *status);
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const;
  void free () override;
#endif
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
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_WORKER_EXPERIMENT
  IOReturn synchronizeNow ();
  IOReturn readWriteNow (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *);
#endif
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = writable && gHost.cache; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeMsdcBlock, IOBlockStorageDevice)

#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_WORKER_EXPERIMENT
// A single host worker serializes verified, immutable window reads. Writes,
// store operations and synchronous barriers retain their original path.
namespace {
struct CompletionSlot;
struct BlockJob {
  BlockJob *next;
  NativeMsdcBlock *owner;
  IOMemoryDescriptor *buffer;
  UInt64 block, count;
  IOStorageAttributes attributes;
  IOStorageCompletion completion;
  CompletionSlot *slot;
  IOReturn completionStatus;
  UInt64 completionBytes;
  bool hasAttributes, completed, ready;
};
struct CompletionSlot { BlockJob *job; bool started, starting; };
// Each queued request reserves a dedicated completion thread. Threads are
// created lazily and bounded; saturation falls back to the original polled
// path for immutable reads rather than surfacing a new capacity I/O error.
// Separate media credits (including the active request) from completion
// threads, whose slots remain reserved until their callback returns.
constexpr unsigned kBlockQueueLimit = 32, kCompletionSlots = 64;
IOLock *gBlockQueueLock = nullptr;
BlockJob *gBlockHead = nullptr, *gBlockTail = nullptr;
CompletionSlot gBlockSlots[kCompletionSlots] = {};
unsigned gBlockQueued = 0, gBlockIoPending = 0, gBlockOutstanding = 0, gBlockDirect = 0;
bool gBlockAccept = false, gBlockRunning = false, gBlockEnabled = false;
bool gBlockPoolShutdown = false, gBlockPoolReady = false, gBlockStartAttempted = false;
IOService *gBlockParent = nullptr;

void blockQueueAppend (BlockJob *job)
{
  job->next = nullptr;
  if (gBlockTail) gBlockTail->next = job; else gBlockHead = job;
  gBlockTail = job;
  ++gBlockQueued;
  IOLockWakeup (gBlockQueueLock, &gBlockHead, false);
}

void blockCapture (void *target, void *, IOReturn status, UInt64 bytes)
{
  BlockJob *job = static_cast<BlockJob *> (target);
  job->completionStatus = status; job->completionBytes = bytes; job->completed = true;
}

void blockCompletionWorker (void *argument, wait_result_t)
{
  CompletionSlot *slot = static_cast<CompletionSlot *> (argument);
  IOLockLock (gBlockQueueLock);
  for (;;) {
    while ((!slot->job || !slot->job->ready) && (!gBlockPoolShutdown || slot->job))
      IOLockSleep (gBlockQueueLock, slot, THREAD_UNINT);
    if (!slot->job && gBlockPoolShutdown) break;
    BlockJob *job = slot->job;
    IOLockUnlock (gBlockQueueLock);
    job->completion.action (job->completion.target, job->completion.parameter,
                            job->completionStatus, job->completionBytes);
    job->buffer->release ();
    job->owner->release ();
    IOLockLock (gBlockQueueLock);
    slot->job = nullptr;
    --gBlockOutstanding;
    IOLockUnlock (gBlockQueueLock);
    IOFree (job, sizeof (*job));
    IOLockLock (gBlockQueueLock);
  }
  IOLockUnlock (gBlockQueueLock);
  gBlockParent->release ();
}

void blockWorker (void *, wait_result_t)
{
  __atomic_store_n (&gIoWorkerThread, current_thread (), __ATOMIC_RELEASE);
  IOLockLock (gBlockQueueLock);
  for (;;) {
    while (!gBlockHead && gBlockAccept) IOLockSleep (gBlockQueueLock, &gBlockHead, THREAD_UNINT);
    if (!gBlockHead && !gBlockAccept) break;
    BlockJob *job = gBlockHead;
    gBlockHead = job->next;
    if (!gBlockHead) gBlockTail = nullptr;
    --gBlockQueued;
    bool cancelled = !gBlockAccept;
    IOLockUnlock (gBlockQueueLock);
    IOStorageCompletion proxy = { job, blockCapture, nullptr };
    IOReturn result = cancelled ? kIOReturnAborted :
      job->owner->readWriteNow (job->buffer, job->block, job->count,
                                job->hasAttributes ? &job->attributes : nullptr, &proxy);
    if (result != kIOReturnSuccess) blockCapture (job, nullptr, result, 0);
    if (!job->completed) blockCapture (job, nullptr, kIOReturnError, 0);
    __atomic_fetch_add (&native_msdc_worker_stats[1], 1, __ATOMIC_RELAXED);
    IOLockLock (gBlockQueueLock);
    --gBlockIoPending; // media credit is independent of callback lifetime
    job->ready = true;
    IOLockWakeup (gBlockQueueLock, job->slot, false);
  }
  __atomic_store_n (&gIoWorkerThread, static_cast<thread_t> (nullptr), __ATOMIC_RELEASE);
  gBlockRunning = false;
  IOLockWakeup (gBlockQueueLock, &gBlockRunning, false);
  IOLockUnlock (gBlockQueueLock);
  gBlockParent->release ();
}

void stopBlockWorker ()
{
  IOLock *lock = __atomic_load_n (&gBlockQueueLock, __ATOMIC_ACQUIRE);
  if (!lock) return;
  IOLockLock (lock);
  gBlockAccept = false; gBlockPoolShutdown = true;
  IOLockWakeup (gBlockQueueLock, &gBlockHead, false);
  for (unsigned i = 0; i < kCompletionSlots; ++i) IOLockWakeup (gBlockQueueLock, &gBlockSlots[i], false);
  IOLockUnlock (lock);
  // Never wait under an upper-layer lock. Every thread retains the provider;
  // queued reads are aborted and in-flight work keeps its resources alive.
}

// Initialize before publication so stop can close admission even while a
// later thread creation is in progress. This lock lives with the host.
bool prepareBlockWorker (IOService *parent)
{
  IOLock *lock = IOLockAlloc ();
  if (!lock) return false;
  gBlockParent = parent;
  gBlockAccept = true;
  __atomic_store_n (&gBlockQueueLock, lock, __ATOMIC_RELEASE);
  IOLockLock (lock);
  if (__atomic_load_n (&gActivationStopped, __ATOMIC_ACQUIRE)) {
    gBlockAccept = false; gBlockPoolShutdown = true;
  }
  __atomic_store_n (&gBlockEnabled, true, __ATOMIC_RELEASE);
  bool accepting = gBlockAccept;
  IOLockUnlock (lock);
  return accepting;
}

bool startBlockWorker (IOService *parent)
{
  if (!__atomic_load_n (&gHost.irqReady, __ATOMIC_ACQUIRE)) return false;
  IOLock *lock = __atomic_load_n (&gBlockQueueLock, __ATOMIC_ACQUIRE);
  if (!lock) return false;
  IOLockLock (lock);
  if (!gBlockAccept || gBlockStartAttempted) { IOLockUnlock (lock); return false; }
  gBlockStartAttempted = true;
  gBlockSlots[0].starting = true;
  parent->retain ();
  IOLockUnlock (lock);
  thread_t completion = nullptr;
  bool created = kernel_thread_start (blockCompletionWorker, &gBlockSlots[0], &completion) == KERN_SUCCESS;
  if (created) thread_deallocate (completion); else parent->release ();
  IOLockLock (lock);
  gBlockSlots[0].starting = false;
  if (created) gBlockSlots[0].started = true;
  if (!created || !gBlockAccept) {
    if (!created) gBlockPoolShutdown = true;
    IOLockWakeup (lock, &gBlockSlots[0], false);
    IOLockUnlock (lock);
    return false;
  }
  // The reservation precedes stop. If stop wins while kernel_thread_start is
  // in progress, the new thread observes closed admission and exits.
  gBlockRunning = true;
  parent->retain ();
  IOLockUnlock (lock);
  thread_t worker = nullptr;
  created = kernel_thread_start (blockWorker, nullptr, &worker) == KERN_SUCCESS;
  if (created) thread_deallocate (worker); else parent->release ();
  IOLockLock (lock);
  if (!created) {
    gBlockRunning = false; gBlockPoolShutdown = true;
    IOLockWakeup (lock, &gBlockSlots[0], false);
  } else if (gBlockAccept) {
    __atomic_store_n (&gBlockPoolReady, true, __ATOMIC_RELEASE);
  }
  bool active = created && gBlockAccept;
  IOLockUnlock (lock);
  return active;
}

// The logical start is recorded under the same lock as stop; operations
// admitted before stop keep the provider alive until their direct path ends.
bool blockDirectBegin ()
{
  IOLockLock (gBlockQueueLock);
  bool allowed = gBlockAccept;
  if (allowed) { ++gBlockDirect; gBlockParent->retain (); }
  IOLockUnlock (gBlockQueueLock);
  return allowed;
}
void blockDirectEnd ()
{
  IOLockLock (gBlockQueueLock);
  --gBlockDirect;
  IOLockUnlock (gBlockQueueLock);
  gBlockParent->release ();
}

class OptionalBlockDirectGuard {
  bool held = false, allowed = true;
public:
  OptionalBlockDirectGuard () {
    if (__atomic_load_n (&gBlockEnabled, __ATOMIC_ACQUIRE)) { allowed = blockDirectBegin (); held = allowed; }
  }
  ~OptionalBlockDirectGuard () { if (held) blockDirectEnd (); }
  bool ok () const { return allowed; }
};

}
#endif

bool NativeMsdcBlock::configure (unsigned index, const RootWindow &entry, uint64_t absoluteSector, bool allowWrites)
{
#if NATIVE_MSDC_VERIFIED
  // Invalidate before even a rejected reconfiguration; preserve lock ordering.
  VerifiedWindowGuard guard (verifiedLock);
  verifiedCache.invalidate ();
  __atomic_add_fetch (&verifiedConfiguration, 1, __ATOMIC_RELEASE);
  if (verifiedActive) online = false; // a failed live reconfiguration must fail closed
  verifiedActive = false;
#endif
  uint64_t count = entry.size / kSector;
  if (!count || absoluteSector < gHost.gpt.userdataFirst || absoluteSector > gHost.gpt.userdataLast ||
      count > gHost.gpt.userdataLast - absoluteSector + 1)
    return false;
  window = index;
  firstSector = absoluteSector;
  sectors = count;
  writable = allowWrites && !(entry.flags & kWindowReadOnly);
#if NATIVE_MSDC_VERIFIED
  manifestReadOnly = (entry.flags & kWindowReadOnly) != 0;
#endif
  strlcpy (label, entry.name, sizeof (label));
  return true;
}

bool NativeMsdcBlock::start (IOService *provider)
{
  if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE) || isInactive ()) return false;
  if (!IOBlockStorageDevice::start (provider)) return false;
  // The provider and its attached children become inactive in termination
  // phase 1. startMatching also rejects an inactive service under IOKit's
  // arbitration lock, even if termination races this check.
  if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE) || isInactive ()) return false;
  online = true;
  registerService ();
  return true;
}

IOReturn NativeMsdcBlock::doSynchronizeCache ()
{
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_WORKER_EXPERIMENT
  if (!__atomic_load_n (&gBlockEnabled, __ATOMIC_ACQUIRE)) return synchronizeNow ();
  if (!blockDirectBegin ()) return kIOReturnNotReady;
  IOReturn result = synchronizeNow ();
  blockDirectEnd ();
  return result;
}

IOReturn NativeMsdcBlock::synchronizeNow ()
{
#endif
  if (!online) return kIOReturnNotReady;
  if (!writable) return kIOReturnSuccess;
  if (gHost.poisoned) return kIOReturnNotReady;
  HostSession session;
  return hostFlush ();
}

IOReturn NativeMsdcBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                            IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_WORKER_EXPERIMENT
  if (__atomic_load_n (&gBlockEnabled, __ATOMIC_ACQUIRE)) {
    IOLockLock (gBlockQueueLock); bool accepting = gBlockAccept; IOLockUnlock (gBlockQueueLock);
    if (!accepting) return kIOReturnNotReady;
  }
#if NATIVE_MSDC_VERIFIED
  if (__atomic_load_n (&gBlockEnabled, __ATOMIC_ACQUIRE) && __atomic_load_n (&gBlockPoolReady, __ATOMIC_ACQUIRE) && verifiedActive && manifestReadOnly && !writable && buffer &&
      buffer->getDirection () == kIODirectionIn) {
    if (!completion || !completion->action) return kIOReturnBadArgument;
    BlockJob *job = static_cast<BlockJob *> (IOMalloc (sizeof (BlockJob)));
    if (!job) {
      if (!blockDirectBegin ()) return kIOReturnNotReady;
      __atomic_fetch_add (&native_msdc_worker_stats[2], 1, __ATOMIC_RELAXED);
      IOReturn result = readWriteNow (buffer, block, count, attributes, completion);
      blockDirectEnd ();
      return result;
    }
    *job = {}; job->owner = this; job->buffer = buffer; job->block = block; job->count = count;
    job->completion = *completion; job->hasAttributes = attributes != nullptr;
    if (attributes) job->attributes = *attributes;
    retain (); buffer->retain ();
    IOLockLock (gBlockQueueLock);
    bool stopping = !gBlockAccept;
    CompletionSlot *slot = nullptr;
    if (!stopping && gBlockIoPending < kBlockQueueLimit) {
      for (unsigned i = 0; i < kCompletionSlots; ++i)
        if (gBlockSlots[i].started && !gBlockSlots[i].job) { slot = &gBlockSlots[i]; break; }
      if (!slot) {
        for (unsigned i = 0; i < kCompletionSlots; ++i) {
          if (gBlockSlots[i].started || gBlockSlots[i].starting) continue;
          CompletionSlot *candidate = &gBlockSlots[i];
          candidate->starting = true;
          gBlockParent->retain ();
          // Thread allocation can block or trigger reclaim. Do not hold the
          // queue mutex or an upper completion's progress hostage to it.
          IOLockUnlock (gBlockQueueLock);
          thread_t thread = nullptr;
          bool created = kernel_thread_start (blockCompletionWorker, candidate, &thread) == KERN_SUCCESS;
          if (created) thread_deallocate (thread); else gBlockParent->release ();
          IOLockLock (gBlockQueueLock);
          candidate->starting = false;
          if (created) { candidate->started = true; if (gBlockAccept && gBlockIoPending < kBlockQueueLimit) slot = candidate; }
          stopping = !gBlockAccept;
          break;
        }
      }
    }
    if (!slot) {
      IOLockUnlock (gBlockQueueLock);
      buffer->release (); release (); IOFree (job, sizeof (*job));
      if (stopping || !blockDirectBegin ()) return kIOReturnNotReady;
      // Verified immutable reads may execute out of queue order; data and
      // writable/flush ordering are unchanged. This is the old polled path.
      __atomic_fetch_add (&native_msdc_worker_stats[2], 1, __ATOMIC_RELAXED);
      IOReturn result = readWriteNow (buffer, block, count, attributes, completion);
      blockDirectEnd ();
      return result;
    }
    slot->job = job; job->slot = slot; ++gBlockOutstanding; ++gBlockIoPending;
    blockQueueAppend (job);
    __atomic_fetch_add (&native_msdc_worker_stats[0], 1, __ATOMIC_RELAXED);
    IOLockUnlock (gBlockQueueLock);
    return kIOReturnSuccess;
  }
#endif // NATIVE_MSDC_VERIFIED
  if (!__atomic_load_n (&gBlockEnabled, __ATOMIC_ACQUIRE)) return readWriteNow (buffer, block, count, attributes, completion);
  if (!blockDirectBegin ()) return kIOReturnNotReady;
  IOReturn result = readWriteNow (buffer, block, count, attributes, completion);
  blockDirectEnd ();
  return result;
}

IOReturn NativeMsdcBlock::readWriteNow (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                         IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
#endif
#if NATIVE_MSDC_VERIFIED
  const uint64_t configuration = verifiedActive ?
    __atomic_load_n (&verifiedConfiguration, __ATOMIC_ACQUIRE) : 0;
  // Cache-off and Data windows retain their original write hot path.
  if (verifiedCacheEnabled && manifestReadOnly && buffer &&
      buffer->getDirection () == kIODirectionOut && verifiedLock) {
    IOLockLock (verifiedLock);
    verifiedCache.invalidate ();
    IOLockUnlock (verifiedLock);
  }
#endif
  if (!buffer || !completion || !completion->action || !count || block >= sectors || count > sectors - block ||
      buffer->getLength () < count * kSector)
    return kIOReturnBadArgument;
  IODirection direction = buffer->getDirection ();
  bool write = direction == kIODirectionOut;
  if (!write && direction != kIODirectionIn) return kIOReturnBadArgument;
#if NATIVE_MSDC_VERIFIED
  if (verifiedActive) {
    if (write) return kIOReturnNotWritable;     // a verified window is served read-only
    if (!online) return kIOReturnNotReady;
    return readVerified (buffer, block, count, completion, configuration);
  }
#endif
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
      result = transfer (firstSector + block + done, blocks, gHost.bounce, false, true);
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

#if NATIVE_MSDC_VERIFIED
// Selects this window's policy and opens it (vr_open, owner key, minimum generation 1). Returns nullptr when the window
// is to be served verified, otherwise the reason it must not be published; *status gets the vr_open result or -1.
const char *NativeMsdcBlock::configureVerification (int *status)
{
  *status = -1;
  if (!verifiedLock) verifiedLock = IOLockAlloc ();
  if (!verifiedLock) return "verified read lock";
  VerifiedWindowGuard guard (verifiedLock);
  verifiedCache.invalidate ();
  __atomic_add_fetch (&verifiedConfiguration, 1, __ATOMIC_RELEASE);
  if (verifiedActive) online = false; // a failed live reconfiguration must fail closed
  verifiedActive = false;
  if (!gPolicy.storage || !gPolicy.key) return "no signed root policy";
  const uint8_t *region = reinterpret_cast<const uint8_t *> (gPolicy.storage);
  uint64_t windowBytes = sectors * kSector;
  unsigned index = 0;
  switch (selectPolicy (region, gPolicy.header, windowBytes, index)) {
  case kPolicySelected: break;
  case kPolicyDuplicate: return "two policy entries have this window's size";
  default: return "no policy entry has this window's size";
  }
  const PolicyEntry &entry = gPolicy.header.entry[index];
  struct vr_view view = {};
  *status = vr_open (region + entry.offset, static_cast<size_t> (entry.bytes), gPolicy.key, 1, &vr_xnu_crypto, &view);
  if (*status != VR_OK) return vrReason (*status);
  if (view.image_bytes != windowBytes) return "signed image size differs from the window";
  if (!verifiedBounce) verifiedBounce = static_cast<uint8_t *> (IOMalloc (kTransferBytes));
  if (!verifiedLock) verifiedLock = IOLockAlloc ();
  if (!verifiedBounce || !verifiedLock) return "verified read buffers";
  verified = view;
  envelope = region + entry.offset;
  writable = false;
  verifiedCacheEnabled = bootArgument ("native-msdc-vcache", 1, 0) == 1;
  verifiedCache.metrics = native_msdc_verified_cache_stats;
  verifiedActive = true;
  return nullptr;
}

IOReturn NativeMsdcBlock::readVerified (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                        IOStorageCompletion *completion, uint64_t configuration)
{
  VerifiedReadKey cacheKey = {};
  const uint64_t windowBytes = sectors * kSector;
  struct Operations {
    IOLock *mutex;
    IOMemoryDescriptor *buffer;
    const vr_view *view;
    uint64_t firstSector;
    NativeMsdcBlock *owner;
    VerifiedReadKey *key;
    uint64_t expectedConfiguration, expectedWindowBytes;
    void lock () {
      IOLockLock (mutex);
      // Bounds were checked before taking this lock: reject a changed window,
      // even with cache disabled, before any read or requester copy.
      if (owner->verifiedConfiguration != expectedConfiguration ||
          owner->sectors * kSector != expectedWindowBytes || !owner->verifiedActive || !owner->online) {
        *key = {};
        return;
      }
      firstSector = owner->firstSector;
      *key = { owner->window, owner->firstSector, owner->sectors * kSector,
               owner->verifiedConfiguration, owner->verified.generation,
               owner->verifiedCacheEnabled && owner->verifiedActive &&
               owner->manifestReadOnly && !owner->writable };
    }
    void unlock () { IOLockUnlock (mutex); }
    int badArgument () { return kIOReturnBadArgument; }
    int ioError () { return kIOReturnIOError; }
    int read (uint64_t windowSector, uint32_t blocks, uint8_t *out)
    {
      if (!owner->verifiedActive || !owner->online) return kIOReturnNotReady;
      // Includes shared host-lock wait, DMA/cache maintenance and bounce copy.
      return verifiedTimedOperation (gRootTimingEnabled, native_msdc_verified_timing_stats, 0,
        uint64_t (blocks) * kSector, rootTimingNow, [&] () {
          HostSession session;
          return transfer (firstSector + windowSector, blocks, out, false, true);
        });
    }
    int verify (uint64_t index, const uint8_t *data, size_t length)
    {
      const int checked = verifiedTimedOperation (gRootTimingEnabled, native_msdc_verified_timing_stats, 3,
        length, rootTimingNow, [&] () { return vr_verify_block (view, index, data, length, &vr_xnu_crypto); });
      if (checked != VR_OK) {
        __atomic_fetch_add (&native_msdc_integrity_failures, 1, __ATOMIC_RELAXED);
        native_msdc_failure = "window block failed its signed hash";
        return kIOReturnNotPrivileged;
      }
      __atomic_fetch_add (&native_msdc_verified_blocks, 1, __ATOMIC_RELAXED);
      return kIOReturnSuccess;
    }
    uint64_t copy (uint64_t offset, const uint8_t *data, uint64_t length)
    {
      return buffer->writeBytes (offset, data, length);
    }
  } operations = { verifiedLock, buffer, &verified, firstSector, this, &cacheKey, configuration, windowBytes };
  VerifiedReadResult outcome = verifiedReadWindow (operations, windowBytes, block, count, verifiedBounce,
                                                   kTransferBytes, &verifiedCache, &cacheKey);
  if (outcome.status == kIOReturnNotPrivileged)
    IOLog ("NativeMsdc: %s: read of sectors %llu+%llu refused, a block failed its signed hash\n", label, block, count);
  completion->action (completion->target, completion->parameter, outcome.status, outcome.done);
  return kIOReturnSuccess;
}

// True when uuid is the volume UUID signed into this window's policy. digest (48 bytes, optional) receives the SHA-384
// of the authenticated envelope (header, signature and hash table), as NativeUfs reports it.
bool NativeMsdcBlock::authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const
{
  if (!uuid || !online || !verifiedActive || !envelope || !verified.hashes) return false;
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) expected[at++] = '-';
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) return false;
  if (manifestDigest) {
    const uint64_t envelopeBytes = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, envelope, envelopeBytes);
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}

void NativeMsdcBlock::free ()
{
  if (verifiedBounce) {
    IOFree (verifiedBounce, kTransferBytes);
    verifiedBounce = nullptr;
  }
  if (verifiedLock) {
    IOLockFree (verifiedLock);
    verifiedLock = nullptr;
  }
  IOBlockStorageDevice::free ();
}

// The root authorization of NativeVirtioBlock (native_verified_block_root_authorized) asks every provider of the root
// media through this weak import: -1 when provider is not an eMMC window, else 1 if its signed volume UUID is uuid and
// 0 if not. NativeUfs defines the same symbol for its windows; a kernel links one of the two.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *digest)
{
  NativeMsdcBlock *block = OSDynamicCast (NativeMsdcBlock, provider);
  if (!block) return -1;
  return block->authenticatesVolume (uuid, digest) ? 1 : 0;
}
#endif // NATIVE_MSDC_VERIFIED
#endif // NATIVE_MSDC_BLOCK_DEVICES

// ---- Controller service ----------------------------------------------------------------------------------------------

class NativeMsdc : public IOService {
  OSDeclareDefaultStructors (NativeMsdc)
#if NATIVE_MSDC_BLOCK_DEVICES
  bool startRoot (bool allowWrites, const char **why);
#endif
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};
OSDefineMetaClassAndStructors (NativeMsdc, IOService)

#if NATIVE_MSDC_BLOCK_DEVICES
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
    if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE)) return false;
    uint32_t blocks = count - done > kMaxBlocks ? kMaxBlocks : uint32_t (count - done);
    HostSession session;
    if (transfer (first + done, blocks, gHost.bounce, false, true) != kIOReturnSuccess) return false;
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

// Stop can run on IOKit's termination workloop with upper locks held. Keep
// this retained thread asleep until then and let it make the provider inactive
// off that workloop. IOKit recursively makes attached children inactive, and
// startMatching refuses inactive children under the arbitration lock.
void publisherCleanupThread (void *argument, wait_result_t)
{
  NativeMsdc *owner = static_cast<NativeMsdc *> (argument);
  uint32_t action;
  while (!(action = __atomic_load_n (&gPublisherCleanup, __ATOMIC_ACQUIRE))) {
    wait_result_t pending = assert_wait (reinterpret_cast<event_t> (&gPublisherCleanup), THREAD_UNINT);
    action = __atomic_load_n (&gPublisherCleanup, __ATOMIC_ACQUIRE);
    if (pending == THREAD_WAITING) {
      if (action) (void) clear_wait (current_thread (), THREAD_AWAKENED);
      else (void) thread_block (THREAD_CONTINUE_NULL);
    }
  }
  if (action == 1) {
    // Resource failure cannot leave a kernel thread retrying forever. Failure
    // is observable; a still-active parent is not claimed to be terminated.
    constexpr unsigned kTerminationAttempts = 8;
    bool terminated = owner->isInactive ();
    for (unsigned attempt = 0; !terminated && attempt < kTerminationAttempts; ++attempt) {
      terminated = owner->terminate (kIOServiceRequired) || owner->isInactive ();
      if (!terminated && attempt + 1 < kTerminationAttempts) IOSleep (250);
    }
    if (!terminated) {
      __atomic_store_n (&native_msdc_publisher_terminate_failed, 1, __ATOMIC_RELEASE);
      IOLog ("NativeMsdc: publisher parent remains active after termination retries\n");
    }
  }
  owner->release ();
}

// Ordered publication: each window before OS waits (up to 30 s) until its APFS volume appears, so SystemOS is
// resolvable when the root mounts. A window that fails its hash or its start is not published; the rest continue.
void publishThread (void *, wait_result_t)
{
#if NATIVE_MSDC_IRQ_WAIT
  __atomic_store_n (&gPublisherThread, current_thread (), __ATOMIC_RELEASE);
#endif
#if NATIVE_MSDC_VERIFIED
  // Every window needs its signed policy before it is published; the region is read here, off the matching thread,
  // since it is tens of MiB. A window without an accepted policy is discarded and never published.
  const char *policyFailure = loadPolicy ();
  if (policyFailure) IOLog ("NativeMsdc: signed root policy unusable: %s; no window is published\n", policyFailure);
  unsigned verifiedWindows = 0;
  for (unsigned index = 0; index < gHost.manifest.count; ++index) {
    if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE)) break;
    NativeMsdcBlock *device = gPending[index];
    if (!device) continue;
    int status = -1;
    const char *why = policyFailure ? policyFailure : device->configureVerification (&status);
    if (why) {
      IOLog ("NativeMsdc: %s not published: %s (vr_open %d)\n", device->windowLabel (), why, status);
      native_msdc_failure = why;
      discardPending (index);
      continue;
    }
    IOLog ("NativeMsdc: %s verified against its signed policy\n", device->windowLabel ());
    ++verifiedWindows;
  }
  native_msdc_verified_windows = verifiedWindows;
  if (!verifiedWindows) freePolicy ();
#endif
  for (unsigned position = 0; position < gPlan.count; ++position) {
    if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE)) break;
    unsigned index = gPlan.order[position];
    NativeMsdcBlock *device = gPending[index];
    if (!device) continue;
    if (gVerifyWindows && !windowMatchesManifest (device)) {
      IOLog ("NativeMsdc: %s does not match its manifest SHA-256; not published\n", device->windowLabel ());
      native_msdc_failure = "window SHA-256 mismatch";
      discardPending (index);
      continue;
    }
    if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE)) break;
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
    for (unsigned tick = 0; tick < 300 && !__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE) &&
         !(ready = volumeBelow (device)); ++tick) IOSleep (100);
    IOLog ("NativeMsdc: %s volume %s\n", device->windowLabel (), ready ? "ready" : "not seen after 30 s");
    device->release ();
  }
  if (__atomic_load_n (&gPublisherStopped, __ATOMIC_ACQUIRE))
    for (unsigned index = 0; index < gHost.manifest.count; ++index) discardPending (index);
  native_msdc_step = 6;
#if NATIVE_MSDC_IRQ_WAIT
  __atomic_store_n (&gPublisherThread, static_cast<thread_t> (nullptr), __ATOMIC_RELEASE);
#endif
  gPendingProvider->release ();
}

} // namespace
#endif // NATIVE_MSDC_BLOCK_DEVICES

IOService *NativeMsdc::probe (IOService *provider, SInt32 *score)
{
  if (!IOService::probe (provider, score) || !listHas (provider->getProperty ("compatible"), "mediatek,mt8167-mmc"))
    return nullptr;
  if (score) *score = 1000;
  return this;
}

#if NATIVE_MSDC_IRQ_WAIT
namespace {
// One activation thread per controller. The lifecycle mutex serializes stop
// with the short registration operation; it is not held during GIC readiness,
// host mutex acquisition, setup allocations, or worker thread creation.
struct IrqActivation { NativeMsdc *owner; IOService *provider; bool worker; };

void activateIrq (void *argument, wait_result_t)
{
  IrqActivation *activation = static_cast<IrqActivation *> (argument);
  bool ready = false;
  for (unsigned attempt = 0; attempt < 600; ++attempt) {
    if (__atomic_load_n (&gActivationStopped, __ATOMIC_ACQUIRE)) break;
    if (armcpu_spi_boot_ready ()) { ready = true; break; }
    IOSleep (100);
  }
  if (ready && !__atomic_load_n (&gActivationStopped, __ATOMIC_ACQUIRE)) {
    {
      // Take the host before the lifecycle gate. In particular, stop never
      // waits for the gate while its owner is waiting for this mutex.
      HostSession session;
      if (!__atomic_load_n (&gActivationStopped, __ATOMIC_ACQUIRE))
        hostEnableIrq (activation->provider, session);
    }
#if NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
    if (!__atomic_load_n (&gActivationStopped, __ATOMIC_ACQUIRE) && activation->worker &&
        __atomic_load_n (&gHost.irqReady, __ATOMIC_ACQUIRE) && !startBlockWorker (activation->owner))
      IOLog ("NativeMsdc: block worker unavailable; polling\n");
#endif
  } else if (!ready && !__atomic_load_n (&gActivationStopped, __ATOMIC_ACQUIRE)) {
    IOLog ("NativeMsdc: GIC readiness timed out; polling\n");
  }
  activation->provider->release ();
  activation->owner->release ();
  IOFree (activation, sizeof (*activation));
}

void scheduleIrqActivation (NativeMsdc *owner, IOService *provider, bool worker)
{
  IrqActivation *activation = static_cast<IrqActivation *> (IOMalloc (sizeof (*activation)));
  if (!activation) { IOLog ("NativeMsdc: IRQ activation allocation failed; polling\n"); return; }
  *activation = { owner, provider, worker };
  owner->retain (); provider->retain ();
  thread_t thread = nullptr;
  if (kernel_thread_start (activateIrq, activation, &thread) != KERN_SUCCESS) {
    provider->release (); owner->release (); IOFree (activation, sizeof (*activation));
    IOLog ("NativeMsdc: IRQ activation thread unavailable; polling\n");
    return;
  }
  thread_deallocate (thread);
}
}
#endif

bool NativeMsdc::start (IOService *provider)
{
#if NATIVE_MSDC_VERIFIED
  gRootTimingEnabled = bootArgument ("native-root-timing", 1, 0) == 1;
#endif
  unsigned root = bootArgument ("native-msdc-root", 2, 0);
  unsigned persist = bootArgument ("native-msdc-persist", 2, 0);
  unsigned dma = bootArgument ("native-msdc-dma", 1, 0);
  if (!root && !persist) return false;   // matched only; the controller is not touched
  gModeTelemetryEnabled = bootArgument ("native-msdc-mode", 1, 0) == 1;
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
#if NATIVE_MSDC_IRQ_WAIT
  bool activate = bootArgument ("native-msdc-irq", 1, 0) == 1;
  if (activate) {
    gActivationLock = IOLockAlloc ();
    if (!gActivationLock) { IOLog ("NativeMsdc: IRQ lifecycle lock unavailable; polling\n"); activate = false; }
  }
#endif
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  bool workerPrepared = activate && root && gHost.manifestValid && prepareBlockWorker (this);
  if (activate && root && gHost.manifestValid && !workerPrepared)
    IOLog ("NativeMsdc: block lifecycle unavailable; polling\n");
#endif
  if (!gHost.manifestValid) fail (manifestReason (native_msdc_manifest_status));
#if NATIVE_MSDC_BLOCK_DEVICES
  else if (root) {
    gVerifyWindows = bootArgument ("native-msdc-verify", 1, 0) == 1;
    if (!startRoot (root == 2, &why)) {
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_WORKER_EXPERIMENT
      stopBlockWorker ();
#endif
      fail (why);
#if NATIVE_MSDC_IRQ_WAIT
      activate = false;
#endif
    }
  }
#else
  else if (root) fail ("native-msdc-root: built without the window block devices (NATIVE_MSDC_BLOCK_DEVICES=0)");
#endif
#if NATIVE_MSDC_IRQ_WAIT
#if NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  if (activate) scheduleIrqActivation (this, provider, workerPrepared);
#else
  if (activate) scheduleIrqActivation (this, provider, false);
#endif
#endif
  return true;
}

#if NATIVE_MSDC_BLOCK_DEVICES
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
  // Start cleanup before admitting asynchronous publication; if allocation
  // fails no child is published and no teardown thread is needed.
  retain ();
  thread_t thread = nullptr;
  if (kernel_thread_start (publisherCleanupThread, this, &thread) != KERN_SUCCESS) {
    release ();
    for (unsigned index = gHost.manifest.count; index;) discardPending (--index);
    *why = "publisher cleanup thread";
    return false;
  }
  thread_deallocate (thread);
  retain ();
  thread = nullptr;
  if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
    release ();
    uint32_t waiting = 0;
    (void) __atomic_compare_exchange_n (&gPublisherCleanup, &waiting, 2, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    thread_wakeup (reinterpret_cast<event_t> (&gPublisherCleanup));
    for (unsigned index = gHost.manifest.count; index;) discardPending (--index);
    *why = "publisher thread";
    return false;
  }
  thread_deallocate (thread);
  native_msdc_step = 5;
  return true;
}
#endif // NATIVE_MSDC_BLOCK_DEVICES

void NativeMsdc::stop (IOService *provider)
{
#if NATIVE_MSDC_BLOCK_DEVICES
  __atomic_store_n (&gPublisherStopped, true, __ATOMIC_RELEASE);
  __atomic_store_n (&gPublisherCleanup, 1, __ATOMIC_RELEASE);
  thread_wakeup (reinterpret_cast<event_t> (&gPublisherCleanup));
#endif
#if NATIVE_MSDC_IRQ_WAIT
  __atomic_store_n (&gActivationStopped, true, __ATOMIC_RELEASE);
  if (gActivationLock) {
    IOLockLock (gActivationLock);
    IOLockUnlock (gActivationLock);
  }
#endif
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  stopBlockWorker ();
#endif
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
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  OptionalBlockDirectGuard guard;
  if (!guard.ok ()) return 0;
#endif
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gHost.ready || !gHost.manifestValid || gHost.poisoned) reason = "no root layout";
  else {
    HostSession session;
    uint64_t first = 0;
    // The store also stays clear of the space the signed root policy region may use after the windows.
    uint64_t protectedBytes = gHost.manifest.imageBytes;
#if NATIVE_MSDC_VERIFIED
    if (policyRegionOffset (gHost.manifest) + kPolicyMaxBytes > protectedBytes)
      protectedBytes = policyRegionOffset (gHost.manifest) + kPolicyMaxBytes;
#endif
    if (gStoreBlocks) reason = "already open";
    else if (const char *refused = storeWindow (gHost.gpt.userdataLast - gHost.gpt.userdataFirst + 1,
                                                protectedBytes, reserveBlocks (), blocks, first))
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
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  OptionalBlockDirectGuard guard;
  if (!guard.ok ()) return 0;
#endif
  return storeTransfer (relative, blocks, static_cast<uint8_t *> (destination), false);
}

extern "C" int native_msdc_store_write (uint64_t relative, uint32_t blocks, const void *source)
{
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  OptionalBlockDirectGuard guard;
  if (!guard.ok ()) return 0;
#endif
  if (storeMode () != 2) return 0;
  // transfer() only reads from the buffer on a write.
  if (!storeTransfer (relative, blocks, static_cast<uint8_t *> (const_cast<void *> (source)), true)) return 0;
  HostSession session;
  return hostFlush () == kIOReturnSuccess;
}

extern "C" int native_msdc_store_sync (void)
{
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  OptionalBlockDirectGuard guard;
  if (!guard.ok ()) return 0;
#endif
  if (storeMode () != 2 || !gStoreBlocks || gHost.poisoned) return 0;
  HostSession session;
  return hostFlush () == kIOReturnSuccess;
}

// Re-read the GPT header and entry array and compare their CRCs with those recorded at start. 0 = unchanged.
extern "C" unsigned native_msdc_store_gpt_check (void)
{
#if NATIVE_MSDC_IRQ_WAIT && NATIVE_MSDC_BLOCK_DEVICES && NATIVE_MSDC_WORKER_EXPERIMENT
  OptionalBlockDirectGuard guard;
  if (!guard.ok ()) return 1;
#endif
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
