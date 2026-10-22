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
