# NativeMsdc

Native eMMC storage driver for the MediaTek MT8167 MSDC controller. It serves the eMMC of the Benesse TAB-A04-BR3
(MSDC0, 512-byte sectors): the windows of the UniXNU root image in the userdata partition as block devices, and the
persistent Data store at the end of userdata.

Current state: compiles against the xnu-10063.141.1 headers; not yet booted. The register sequences it uses read and
wrote the eMMC of the TAB-A04-BR3 in the earlier a04br3 XNU port.

Linked into XNU by `unixnu build kernel` when the board's defconfig sets `CONFIG_XNU_NATIVE_MSDC=y`.

## Matching

The personality (patches/xnu/native-msdc-personality.patch of UniXNU, compiled in with `XNU_NATIVE_MSDC=1`) matches
every `IOPlatformDevice`; `probe` accepts the node whose `compatible` list contains
`"mediatek,mt8167-mmc"`. The register window is the node's `reg` as resolved by the platform expert, checked against
`reg` and the `arm-io` `ranges`. Only one controller is served.

Without a boot argument the driver matches and stops: the controller is not touched.

| Boot argument | Effect |
| --- | --- |
| `native-msdc-root=1` | Publish the windows of the userdata manifest as read-only block devices |
| `native-msdc-root=2` | As 1; a window whose manifest entry clears the read-only flag also accepts writes |
| `native-msdc-persist=1` | Persistent Data store, read-only (proves the window) |
| `native-msdc-persist=2` | Persistent Data store, read-write |
| `native-msdc-reserve=<MiB>` | Space at the end of userdata the store stays clear of, 1024 by default |
| `native-msdc-dma=1` | Descriptor DMA for reads, used only after it reads the same bytes as PIO |
| `native-msdc-verify=1` | Hash each window against its manifest SHA-256 before publishing it |

## Controller

The boot firmware leaves the card initialized in the transfer state (RCA 1). The driver keeps the inherited clock,
bus width, voltage and sampling, and never resets the controller. For each request it masks the controller
interrupts and selects PIO, then restores both. Commands are polled.

- Start: SEND_STATUS must report the transfer state; EXT_CSD gives the capacity (SEC_COUNT), the cache state
  (CACHE_CTRL) and must show the user area selected (PARTITION_ACCESS 0).
- Reads: CMD17, or CMD23 + CMD18 for up to 128 sectors. With `native-msdc-dma=1` the same commands use one
  GPD/BD descriptor pair and a private buffer below 4 GiB. If the DMA engine does not stop, the transport is
  quarantined and the buffer is never reused.
- Writes: CMD24, or CMD23 + CMD25, PIO, then SEND_STATUS until the card has finished programming.
- Forced unit access and synchronize: SWITCH to FLUSH_CACHE when the card cache is on.

## Root windows

The driver reads the primary GPT at LBA 1, checks both CRCs and the partition bounds, and finds the one partition
named `userdata`. Its first 4096 bytes hold the SHNGROOT manifest (UniXNU docs, concepts/userdata). Each window
becomes a `NativeMsdcBlock` (`IOBlockStorageDevice`, 512-byte blocks). SystemOS is published first and
OS last; each window before OS waits up to 30 s for its APFS volume, so SystemOS is present when the root mounts.
XNU then selects the root through `/chosen/root-matching`.

## Verified root (NATIVE_VERIFIED_BLOCK=1)

iPadOS APFS requires the root volume to be authenticated (`rootvp not authenticated after mounting` otherwise). Built
with `-DNATIVE_VERIFIED_BLOCK=1` and `kits/VerifiedRoot/include` (`build-arm-driver.py NativeMsdc --verified-root`),
the windows are served only through owner-signed VerifiedRoot policies. The default build is unchanged and does not
verify. This is independent of `native-msdc-verify`, which compares whole windows with the manifest SHA-256.

The policies are stored in userdata after the windows, at the end of the window that ends farthest into userdata
rounded up to 1 MiB:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `A04RPOL1` |
| 8 | 4 | version, 1 |
| 12 | 4 | count of entries, 1..16 |
| 16 | 8 | total_bytes, the whole region including the header, at most 256 MiB |
| 24 | 16 x count | entries: u64 offset from the start of the region, u64 bytes |
| 4064 | 32 | SHA-256 of the 4096-byte header with these 32 bytes taken as zero |

All fields are little endian. Each entry is one VROOT01 envelope as written by `kits/VerifiedRoot/tools/manifest.py`;
entries lie in [4096, total_bytes) and do not overlap.

Before publishing, the publisher thread reads the whole region into kernel memory (`kmem_alloc`, `KMA_KOBJECT |
KMA_DATA`), checks the header again from that copy, appends a copy of `verified_root_key` and makes both read-only
with `vm_map_protect`. For each window it selects the one entry whose image size equals the window size (none or
two refuse the window) and opens it with `vr_open` (minimum generation 1, `vr_xnu_crypto`). Only a window whose
envelope opens and whose signed image size equals the window is published; the reason for any other is logged and the
window is discarded. The publication order stays SystemOS first and OS last.

A verified window reads whole 4096-byte blocks of the window (the last one may be shorter), checks each with
`vr_verify_block` and only then copies the requested sectors. A block that fails returns `kIOReturnNotPrivileged` and
counts in `native_msdc_integrity_failures`. Writes are refused. `native_ufs_block_authenticates` (the weak import of
NativeVirtioBlock's root authorization) is defined for these windows: -1 for another provider, 1 when the UUID is the
signed volume UUID, otherwise 0; it also returns the SHA-384 of the authenticated envelope, as NativeUfs does. NativeUfs
defines the same symbol, so a kernel links only one of the two verified drivers. The link needs `verified_root_key`,
`vr_open`, `vr_verify_block` and `vr_xnu_crypto` from a separate archive.

The persistent store stays clear of 256 MiB after the policy region start in this build.

## Persistent store

NativeRamBlock persists the Data volume's changed pages through a store window (PersistOverlay.h). NativeMsdc
provides that interface as `native_msdc_store_{mode,open,read,write,sync,gpt_check}`, in 4096-byte blocks, at the end
of userdata below the reserve, clear of the root image plus 4 MiB. A write returns once the blocks are written and
the card cache is flushed. `gpt_check` re-reads the GPT and compares both CRCs with those seen at start.

NativeRamBlock imports the store under the `native_ufs_store_*` names. A board without NativeUfs compiles NativeMsdc
with `NATIVE_MSDC_UFS_STORE_ABI=1` to provide those names as well; linking it next to NativeUfs fails on the duplicate
symbols.

## Diagnostics

`native_msdc_step`, `native_msdc_failure`, `native_msdc_manifest_status`, `native_msdc_dma` and the transfer counters
are exported for boot pages and panic reports, and with the verified root `native_msdc_policy_status`,
`native_msdc_verified_windows`, `native_msdc_verified_blocks` and `native_msdc_integrity_failures`. Messages go to
`IOLog` with the prefix `NativeMsdc:`.

## Tests

`tests/run.sh` builds and runs the host tests of the pure logic in `NativeMsdc.h` (policy region location and header,
policy selection, verified window reads including the short last block) with AddressSanitizer and UBSan.

## Build

`scripts/build-arm-driver.py NativeMsdc` of the board repository compiles `NativeMsdc.cpp` with the recorded XNU
flags and the IOStorageFamily headers (`build/storage-abi/include`) into `build/native-msdc/libNativeMsdc.a`.
`scripts/build-xnu-4k-baseline.sh` links it and compiles the personality when `XNU_NATIVE_MSDC=1`.

The window block devices subclass the original `IOBlockStorageDevice` of the prelinked IOStorageFamily. As for the
other block transports, the kernel link then needs the audited aliases of that class:
`build-native-virtio-bindings.py --component NativeMsdc --directory build/native-msdc` (input:
`build/storage-abi/interface-audit.json`). A board without that audit compiles with `NATIVE_MSDC_BLOCK_DEVICES=0`: the
driver then serves the persistent store only, references no original storage symbol and links on its own;
`native-msdc-root` reports that the windows were not built.

## Credits

- Command sequences, register bits, descriptor DMA and the GPT handling: `bsd/dev/a04br3_msdc.c` and
  `a04br3_msdc_dma.h` of [xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3), adapted from the
  MT8167 eMMC driver of [mouseos/edk2-mtk](https://github.com/mouseos/edk2-mtk).
- Structure, manifest rules, publication order and the store interface: NativeUfs and NativeRamBlock of
  [UniXNU](https://github.com/UniXNU/UniXNU).

### Optional authenticated root snapshot cache

`native-msdc-vcache=1` (exact value 1; absent/other values disable it) enables
reuse of the last fully authenticated chunk, at most 64 KiB, in each verified
read-only manifest window. Requires the existing `NATIVE_VERIFIED_BLOCK=1` build.
No board default or signed policy changes are needed. Data/overlay and writable
manifest windows are excluded. The existing per-window lock protects the bounce
buffer and cache state; no new cache lock or allocation is used.

Keys include manifest window identity, absolute first sector, window size,
configuration epoch, signed policy generation and the current bounce buffer.
Every miss invalidates before reading; only a completely verified chunk becomes
valid. Writes (including refused/invalid write requests with an output buffer)
and all reconfiguration attempts invalidate first. Padding is never cached.

The exported `native_msdc_verified_cache_stats[3]` contains aggregate cache hits,
misses and authenticated bytes reused, respectively. These are cumulative atomic
counters with no hot-path logging. Hits count reused segments, not whole I/O
requests; a cross-range request can have both a hit and a miss. Reused bytes count
bytes actually copied to the requester. A miss can end in an I/O/hash failure.
Performance is unverified until measured on hardware; this cache is default-off.
Host validation: `sh drivers/NativeMsdc/tests/run-cache.sh`. The host fixture also exercises concurrent reads and cache invalidation against a mutex-backed model; it does not validate IOKit, DMA, device lifetime, or concurrent physical media mutation.

Fresh unverified/Data windows do not allocate a verification lock, and cache-off
or Data writes do not take the cache invalidation lock. The read entry captures
a configuration epoch before validation and rechecks epoch/window size under the
window lock; reconfigured/offline reads fail before I/O or copying. A failed live
reconfiguration leaves the verified window offline rather than falling back to
unauthenticated reads. A04BR3 optionally reports nonzero aggregate counters on
its existing process-report thread, no more often than once per 60 seconds.

### Optional root startup timing

`native-root-timing=1` (exactly 1, default off) enables atomic cumulative
`native_msdc_verified_timing_stats[6]`: read calls, requested read bytes,
read nanoseconds, verify calls, verify bytes, verify nanoseconds. Read time
includes host-lock wait, transport/cache maintenance and bounce copying;
verify time covers `vr_verify_block` only. Failed attempts are counted.
Disabled operations read no clock and update no timing counters. There is no
per-I/O timing log. `native_root_timing_snapshot` supplies an optional aggregate
snapshot to the kernel's separate default-off shared-region timing diagnostics.
These are system-wide intervals, not per-PID CPU attribution. Cache hits do not
perform or count a new transport read/hash. Test: `sh tests/run-timing.sh`.

### Optional inherited bus-mode telemetry

`native-msdc-mode=1` (exactly 1, default off) emits four newline-terminated
records, each below 200 bytes, once during successful controller bring-up.
It reads controller CFG/IOCON/SDC_CFG/PAD_TUNE0/1 and the existing successful
CMD8 EXT_CSD buffer: BUS_WIDTH[183], HS_TIMING[185], revision[192],
CARD_TYPE[196] and DRIVER_STRENGTH[197]. CARD_TYPE is capability, not active
mode. BUS_WIDTH[183] is not readable: its raw byte is retained only and labelled
`not_readable`; card width, card DDR and host/card width agreement are unknown
and are not derived. Reserved controller width encodings report 0 bits.
HS_TIMING[185] is readable; its raw byte and low timing nibble are reported.
Reference: official mmc-utils commit
`4afc8c81685825461677311e79059cda15fba9d8`, `mmc_cmds.c:1018-1020`,
which prints HS_TIMING[185] and explicitly marks BUS_WIDTH[183] not readable.

A temporary read-only TOPCKGEN mapping reports register 0x10000000 and the
MSDC0 mux selector [13:11]; mapping failure is diagnostic-only. No register is
written and no extra card command/read, reset, tuning or mode transition is
performed. The mapping is released before logging. Absent/invalid flags perform
none of these extra reads, mappings or logs. Existing transport/verification
counters remain controlled by `native-root-timing=1`; no duplicate timing path
is added. Test: `python3 tests/check-mode-telemetry.py` (actual helper extraction,
strict gate, literal decoding, allocation/map failures and console bounds).

Register evidence: MT8167 vendor Linux 4.4
`drivers/mmc/host/mediatek/mt8167/msdc_reg.h`, `sd.c:msdc_set_mclk`, and
`drivers/clk/mediatek/clk-mt8167.c:msdc0_parents` / `CLK_TOP_MSDC0_SEL`.
CFG 0x02000199 means CKMOD=0, CKDIV=1, clock-stable=1; it does not establish
source frequency or bus width. The MT8167-specific clock-parent table, rather
than a generic MSDC source-frequency table, applies. Live PLL frequency remains
unmeasured and is explicitly reported as such.

The A04BR3 EDK2 `SdCardBlockIoDxe.c:SdTuneTransferMode` reference configures
8-bit SDR, HS_TIMING=1 and requests 52 MHz. Its configured 188 MHz source and
`SdSetClock` divider imply 47 MHz, conditional on that source actually being
188 MHz. This is source configuration evidence, not a measurement of the
current inherited LK state. Vendor Linux also implements DDR52/HS200/HS400,
but changes sampling/tuning paths (`msdc_init_tune_path`, autok/CMD21), beyond
clock-divider changes. No higher-speed transition is implemented here: first
capture controller width, readable card timing and mux, then qualify a complete supported transition
with baseline/candidate byte comparison before publication. Existing DMA/PIO
comparison and publication rules remain unchanged.

Measured whole-file `vn_rdwr` throughput is not card-link bandwidth. Compare
its interval with the existing same-boot transport and verified-root SHA
counters; APFS/VM and other caller work remain outside those driver counters.
