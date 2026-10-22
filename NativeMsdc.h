// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// NativeMsdc: MT8167 MSDC register map, eMMC command encoding and the pure userdata layout logic.
//
// No IOKit, no MMIO and no allocation here, so the same code is used by the kernel driver and by host tests.
// The includer provides uint8_t/uint32_t/uint64_t, size_t, memcpy, memset, memcmp and strcmp.
//
// Register usage and command sequences come from the a04br3 XNU port (bsd/dev/a04br3_msdc.c and
// a04br3_msdc_dma.h of github.com/mouseos/xnu_kernel_benesse_a04br3), which read and wrote the eMMC of the
// TAB-A04-BR3 with them, and from the MSDC driver of the a04br3 EDK II port it was adapted from.

namespace NativeMsdcHw {

// ---- Registers (offsets from the MSDC base) ---------------------------------------------------------------------
enum : unsigned {
  kMsdcCfg = 0x00,     // MSDC_CFG: bit 3 PIO mode
  kMsdcIntr = 0x0c,    // MSDC_INT: write 1 to clear
  kMsdcIntEn = 0x10,   // MSDC_INTEN
  kMsdcFifoCs = 0x14,  // MSDC_FIFOCS: RX count [7:0], TX count [23:16], clear bit 31
  kMsdcTxData = 0x18,
  kMsdcRxData = 0x1c,
  kSdcCmd = 0x34,
  kSdcArg = 0x38,
  kSdcSts = 0x3c,      // bit 0 SDC busy, bit 1 command busy
  kSdcResp0 = 0x40,
  kSdcBlkNum = 0x50,
  kDmaSaHigh = 0x8c,   // MSDC_DMA_SA_HIGH4BIT
  kDmaSa = 0x90,
  kDmaCtrl = 0x98,     // bit 0 start, bit 1 stop, bit 8 descriptor mode, bit 10 last buffer, [14:12] burst
  kDmaCfg = 0x9c,      // bit 0 active, bit 1 descriptor checksum
  kDmaLength = 0xa8,
};

constexpr uint32_t kCfgPio = 1u << 3;
constexpr uint32_t kIntCmdReady = 1u << 8;
constexpr uint32_t kIntCmdErrors = (1u << 9) | (1u << 10);       // command timeout, response CRC
constexpr uint32_t kIntXferDone = 1u << 12;
constexpr uint32_t kIntDataErrors = (1u << 14) | (1u << 15);     // data timeout, data CRC
constexpr uint32_t kIntDmaErrors = 7u << 17;                     // GPD/BD checksum, DMA queue
constexpr uint32_t kFifoClear = 1u << 31;
constexpr uint32_t kStsSdcBusy = 1u << 0;
constexpr uint32_t kStsCmdBusy = 1u << 1;
constexpr uint32_t kDmaCtrlStart = 1u << 0;
constexpr uint32_t kDmaCtrlStop = 1u << 1;
constexpr uint32_t kDmaCtrlDescriptor = 1u << 8;
constexpr uint32_t kDmaCtrlLastBuffer = 1u << 10;
constexpr uint32_t kDmaCtrlBurstMask = 7u << 12;
constexpr uint32_t kDmaCtrlBurst64 = 6u << 12;
constexpr uint32_t kDmaCfgActive = 1u << 0;
constexpr uint32_t kDmaCfgChecksum = 1u << 1;
constexpr unsigned kFifoBytes = 128;

// ---- eMMC ---------------------------------------------------------------------------------------------------------
constexpr unsigned kSector = 512;
constexpr unsigned kMaxBlocks = 128;               // one command moves at most 64 KiB
constexpr uint32_t kRca = 1;                       // the relative card address LK assigns to the eMMC
constexpr uint32_t kStatusArgument = kRca << 16;   // CMD13 SEND_STATUS
// SWITCH (CMD6), write byte, EXT_CSD[32] FLUSH_CACHE = 1.
constexpr uint32_t kFlushCacheArgument = (3u << 24) | (32u << 16) | (1u << 8);
// R1 error bits; the current state and READY_FOR_DATA are not errors.
constexpr uint32_t kR1Errors = 0xfdffa080u;

enum : unsigned {
  kCmdSwitch = 6, kCmdSendExtCsd = 8, kCmdStop = 12, kCmdStatus = 13, kCmdReadSingle = 17, kCmdReadMulti = 18,
  kCmdSetBlockCount = 23, kCmdWriteSingle = 24, kCmdWriteMulti = 25,
};

enum class Data : unsigned { None, ReadSingle, WriteSingle, ReadMulti, WriteMulti };

// SDC_CMD: opcode [5:0], response type [9:7] (1 = R1, 7 = R1b), STOP bit 14, data type [12:11]
// (1 single, 2 multiple), write bit 13, block length [27:16].
constexpr uint32_t commandWord (unsigned opcode, Data data)
{
  uint32_t word = opcode | ((opcode == kCmdSwitch || opcode == kCmdStop ? 7u : 1u) << 7);
  if (opcode == kCmdStop) word |= 1u << 14;
  if (data != Data::None) {
    bool multi = data == Data::ReadMulti || data == Data::WriteMulti;
    word |= (multi ? 2u : 1u) << 11;
    word |= uint32_t (kSector) << 16;
    if (data == Data::WriteSingle || data == Data::WriteMulti) word |= 1u << 13;
  }
  return word;
}
static_assert (commandWord (kCmdReadMulti, Data::ReadMulti) == (18u | (1u << 7) | (2u << 11) | (512u << 16)), "CMD18");
static_assert (commandWord (kCmdWriteSingle, Data::WriteSingle) ==
               (24u | (1u << 7) | (1u << 11) | (1u << 13) | (512u << 16)), "CMD24");
static_assert (commandWord (kCmdStop, Data::None) == (12u | (7u << 7) | (1u << 14)), "CMD12");

// CMD12 and CMD13 may run while the data line is busy; every other command waits for the whole SDC.
constexpr uint32_t busyMask (unsigned opcode)
{
  return opcode == kCmdStop || opcode == kCmdStatus ? kStsCmdBusy : kStsSdcBusy;
}

constexpr bool cardInTransfer (uint32_t status)
{
  return (status & (1u << 8)) && ((status >> 9) & 15u) == 4u;
}

// ---- Little-endian fields and the GPT CRC ---------------------------------------------------------------------------
static inline uint32_t le32 (const uint8_t *at)
{
  return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
}

static inline uint64_t le64 (const uint8_t *at)
{
  return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
}

// Reflected IEEE CRC32; header = true zeroes the header CRC field (bytes 16..19) while summing.
static inline uint32_t gptCrc32 (const uint8_t *data, size_t bytes, bool header = false)
{
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= header && i >= 16 && i < 20 ? 0 : data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// ---- GPT (512-byte sectors) -----------------------------------------------------------------------------------------
constexpr uint32_t kGptHeaderLba = 1;
constexpr uint32_t kGptMaxEntries = 128;
constexpr uint32_t kGptEntryBytes = 128;
constexpr uint32_t kGptMaxEntrySectors = kGptMaxEntries * kGptEntryBytes / kSector;

struct GptLayout {
  uint64_t entriesLba;
  uint32_t entryCount, entrySectors, headerCrc, entriesCrc;
  uint64_t firstUsable, lastUsable;
  uint64_t userdataFirst, userdataLast;   // inclusive, 512-byte sectors
};

// Checks the primary header read from LBA 1. Returns nullptr or the reason it is unusable.
static inline const char *gptCheckHeader (const uint8_t *sector, uint64_t diskSectors, GptLayout &gpt)
{
  gpt = {};
  if (memcmp (sector, "EFI PART", 8)) return "GPT signature";
  uint32_t headerBytes = le32 (sector + 12);
  if (le32 (sector + 8) != 0x00010000u || headerBytes < 92 || headerBytes > kSector || le32 (sector + 20) ||
      gptCrc32 (sector, headerBytes, true) != le32 (sector + 16))
    return "GPT header CRC/revision";
  if (le64 (sector + 24) != kGptHeaderLba) return "GPT header location";
  gpt.firstUsable = le64 (sector + 40);
  gpt.lastUsable = le64 (sector + 48);
  gpt.entriesLba = le64 (sector + 72);
  gpt.entryCount = le32 (sector + 80);
  gpt.entriesCrc = le32 (sector + 88);
  gpt.headerCrc = le32 (sector + 16);
  if (le32 (sector + 84) != kGptEntryBytes || !gpt.entryCount || gpt.entryCount > kGptMaxEntries)
    return "GPT entry geometry";
  gpt.entrySectors = (gpt.entryCount * kGptEntryBytes + kSector - 1) / kSector;
  if (gpt.entriesLba < 2 || gpt.entriesLba > gpt.firstUsable || gpt.firstUsable - gpt.entriesLba < gpt.entrySectors ||
      gpt.lastUsable < gpt.firstUsable || gpt.lastUsable >= diskSectors)
    return "GPT usable range";
  return nullptr;
}

static inline bool gptEntryUsed (const uint8_t *entry)
{
  uint8_t used = 0;
  for (unsigned i = 0; i < 16; ++i) used |= entry[i];
  return used != 0;
}

// The name is UTF-16LE at byte 56, at most 36 code units.
static inline bool gptEntryNamed (const uint8_t *entry, const char *name)
{
  unsigned i = 0;
  for (; name[i]; ++i) {
    if (i >= 36 || entry[56 + 2 * i] != uint8_t (name[i]) || entry[57 + 2 * i]) return false;
  }
  return i == 36 || (!entry[56 + 2 * i] && !entry[57 + 2 * i]);
}

// Checks the entry array (gpt.entrySectors sectors) and finds the one partition named "userdata".
static inline const char *gptFindUserdata (const uint8_t *entries, GptLayout &gpt)
{
  if (gptCrc32 (entries, size_t (gpt.entryCount) * kGptEntryBytes) != gpt.entriesCrc) return "GPT entries CRC";
  bool found = false;
  for (uint32_t index = 0; index < gpt.entryCount; ++index) {
    const uint8_t *entry = entries + index * kGptEntryBytes;
    if (!gptEntryUsed (entry)) continue;
    uint64_t first = le64 (entry + 32), last = le64 (entry + 40);
    if (first < gpt.firstUsable || last < first || last > gpt.lastUsable) return "GPT partition bounds";
    for (uint32_t prior = 0; prior < index; ++prior) {
      const uint8_t *other = entries + prior * kGptEntryBytes;
      if (gptEntryUsed (other) && first <= le64 (other + 40) && le64 (other + 32) <= last) return "GPT partition overlap";
    }
    if (!gptEntryNamed (entry, "userdata")) continue;
    if (found) return "duplicate userdata partition";
    gpt.userdataFirst = first;
    gpt.userdataLast = last;
    found = true;
  }
  return found ? nullptr : "no userdata partition";
}

// ---- SHNGROOT manifest at the start of userdata (UniXNU docs: concepts/userdata) ----------------------------------
constexpr unsigned kManifestBytes = 4096;
constexpr unsigned kManifestHashed = 4064;    // the manifest SHA-256 at 4064 covers bytes 0..4063
constexpr unsigned kMaxWindows = 4;
constexpr uint32_t kWindowReadOnly = 1u << 0;

struct RootWindow {
  char name[32];
  uint64_t offset;      // bytes from the start of userdata
  uint64_t size;        // bytes
  uint32_t flags;
  uint8_t sha256[32];
};

struct RootManifest {
  uint64_t imageBytes;
  unsigned count;
  RootWindow window[kMaxWindows];
};

enum ManifestStatus : unsigned {
  kManifestOk = 0, kManifestNoMagic = 1, kManifestVersion = 2, kManifestHash = 3, kManifestBounds = 4,
  kManifestNames = 5, kManifestOverlap = 6, kManifestAttributes = 7,
};

// header: the first 4096 bytes of userdata; digest: SHA-256 of its first 4064 bytes (computed by the caller).
// Same acceptance rules as NativeUfs (RootLayout.h), except that a window may also leave the read-only flag clear.
static inline unsigned parseManifest (const uint8_t *header, const uint8_t *digest, uint64_t partitionBytes,
                                      RootManifest &manifest)
{
  manifest = {};
  if (memcmp (header, "SHNGROOT", 8)) return kManifestNoMagic;
  uint32_t version = le32 (header + 8), count = le32 (header + 12);
  uint64_t imageBytes = le64 (header + 16);
  if (version != 1 || !count || count > kMaxWindows) return kManifestVersion;
  if (memcmp (digest, header + kManifestHashed, 32)) return kManifestHash;
  if (imageBytes < 4096 || (imageBytes & 4095) || imageBytes > partitionBytes) return kManifestBounds;
  for (uint32_t index = 0; index < count; ++index) {
    const uint8_t *entry = header + 24 + 128 * index;
    uint64_t offset = le64 (entry + 32), size = le64 (entry + 40);
    uint32_t blockSize = le32 (entry + 48), flags = le32 (entry + 52);
    if ((offset & 4095) || offset < 4096 || (size & 511) || !size || offset >= imageBytes || size > imageBytes - offset ||
        ((size + 4095) & ~4095ULL) > imageBytes - offset)
      return kManifestBounds;
    if (blockSize != kSector || (flags & ~kWindowReadOnly)) return kManifestAttributes;
    unsigned length = 0;
    while (length < 32 && entry[length] >= 0x20 && entry[length] <= 0x7e) ++length;
    if (!length || length == 32 || entry[length]) return kManifestNames;
    RootWindow &window = manifest.window[index];
    memcpy (window.name, entry, 32);
    window.name[31] = 0;
    for (uint32_t prior = 0; prior < index; ++prior) {
      const RootWindow &other = manifest.window[prior];
      if (!strcmp (window.name, other.name)) return kManifestNames;
      if (offset < other.offset + ((other.size + 4095) & ~4095ULL) && other.offset < offset + ((size + 4095) & ~4095ULL))
        return kManifestOverlap;
    }
    window.offset = offset;
    window.size = size;
    window.flags = flags;
    memcpy (window.sha256, entry + 56, 32);
  }
  manifest.imageBytes = imageBytes;
  manifest.count = count;
  return kManifestOk;
}

// Publication order: SystemOS first (the owner-trust code resolves it right after the root mount), the other windows,
// then OS (the root volume) last. OS is required; SystemOS is optional.
struct RootPlan { unsigned count, order[kMaxWindows]; };

static inline bool buildRootPlan (const RootManifest &manifest, RootPlan &plan)
{
  plan = {};
  unsigned os = manifest.count, system = manifest.count;
  for (unsigned i = 0; i < manifest.count; ++i) {
    if (!strcmp (manifest.window[i].name, "OS")) os = i;
    if (!strcmp (manifest.window[i].name, "SystemOS")) system = i;
  }
  if (os == manifest.count) return false;
  if (system != manifest.count) plan.order[plan.count++] = system;
  for (unsigned i = 0; i < manifest.count; ++i)
    if (i != os && i != system) plan.order[plan.count++] = i;
  plan.order[plan.count++] = os;
  return true;
}

// ---- Persistent store window (4096-byte blocks, partition-relative) -------------------------------------------------
// The consumer (NativeRamBlock's overlay) asks for a window of "blocks" at the end of userdata, below a reserve of
// "reserveBlocks". Refused unless it stays clear of the root image plus a 4 MiB guard (as native_ufs_store_open).
constexpr uint64_t kStoreBlock = 4096;
constexpr uint64_t kSectorsPerStoreBlock = kStoreBlock / kSector;
constexpr uint64_t kStoreGuardBlocks = 1024;

static inline const char *storeWindow (uint64_t partitionSectors, uint64_t imageBytes, uint64_t reserveBlocks,
                                       uint64_t blocks, uint64_t &firstBlock)
{
  uint64_t partition = partitionSectors / kSectorsPerStoreBlock;
  uint64_t imageBlocks = (imageBytes + kStoreBlock - 1) / kStoreBlock;
  if (!blocks || reserveBlocks >= partition || blocks > partition - reserveBlocks) return "size";
  if (partition - reserveBlocks - blocks < imageBlocks + kStoreGuardBlocks) return "overlaps the root image";
  firstBlock = partition - reserveBlocks - blocks;
  return nullptr;
}

// ---- Descriptor DMA (reads) ------------------------------------------------------------------------------------------
// One GPD (plus the terminating GPD) and one BD describing a single physically contiguous buffer below 4 GiB.
struct DmaGpd { uint32_t info, next, pointer, length, argument, blocks, command; };
struct DmaBd { uint32_t info, next, pointer, length; };
static_assert (sizeof (DmaGpd) == 28, "GPD layout");
static_assert (sizeof (DmaBd) == 16, "BD layout");

static inline uint8_t dmaChecksum (const void *descriptor)
{
  const uint8_t *bytes = static_cast<const uint8_t *> (descriptor);
  unsigned sum = 0;
  for (unsigned i = 0; i < 16; ++i) sum += bytes[i];
  return uint8_t (255u - sum);
}

static inline bool dmaDescriptors (DmaGpd *gpd, DmaBd *bd, uint64_t gpdPhys, uint64_t bdPhys, uint64_t bufferPhys,
                                   unsigned bytes)
{
  if (!bytes || bytes > kMaxBlocks * kSector || (bytes & (kSector - 1)) || (gpdPhys & 15) || (bdPhys & 15) ||
      (bufferPhys & 63) || gpdPhys > 0xffffffffULL - 2 * sizeof (DmaGpd) || bdPhys > 0xffffffffULL - sizeof (DmaBd) ||
      bufferPhys > 0x100000000ULL - bytes)
    return false;
  memset (gpd, 0, 2 * sizeof (*gpd));
  memset (bd, 0, sizeof (*bd));
  bd->info = 1;                                  // last BD
  bd->pointer = uint32_t (bufferPhys);
  bd->length = bytes;
  bd->info |= uint32_t (dmaChecksum (bd)) << 8;
  gpd[0].info = 3;                               // hardware owned, BD present
  gpd[0].next = uint32_t (gpdPhys + sizeof (DmaGpd));
  gpd[0].pointer = uint32_t (bdPhys);
  gpd[0].info |= uint32_t (dmaChecksum (gpd)) << 8;
  gpd[1].info = uint32_t (dmaChecksum (gpd + 1)) << 8;
  return true;
}

} // namespace NativeMsdcHw
