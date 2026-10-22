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
are exported for boot pages and panic reports. Messages go to `IOLog` with the prefix `NativeMsdc:`.

## Build

`scripts/build-arm-driver.py NativeMsdc` of the board repository compiles `NativeMsdc.cpp` with the recorded XNU
flags and the IOStorageFamily headers (`build/storage-abi/include`) into `build/native-msdc/libNativeMsdc.a`.
`scripts/build-xnu-4k-baseline.sh` links it and compiles the personality when `XNU_NATIVE_MSDC=1`.

## Credits

- Command sequences, register bits, descriptor DMA and the GPT handling: `bsd/dev/a04br3_msdc.c` and
  `a04br3_msdc_dma.h` of [xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3), adapted from the
  MT8167 eMMC driver of [mouseos/edk2-mtk](https://github.com/mouseos/edk2-mtk).
- Structure, manifest rules, publication order and the store interface: NativeUfs and NativeRamBlock of
  [UniXNU](https://github.com/UniXNU/UniXNU).
