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
Host validation: `sh drivers/NativeMsdc/tests/run-cache.sh`.

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

### Experimental DMA interrupt wait

`NATIVE_MSDC_IRQ_WAIT=1` compiles an experimental interrupt wait. It remains
inactive unless both `native-msdc-dma=1` and `native-msdc-irq=1` are supplied
and the board exposes `msdc-irq-spi`, `msdc-sysirq-base`, and `msdc-irq-low`.
The board must provide the ArmCpu checked SYSIRQ/SPI service, a usable GICv2
non-secure SPI and writable SYSIRQ polarity. Its SYSIRQ bank is shared; all
writers must use the common ArmCpu controller synchronization.
The driver attaches permanently for its host lifetime and only accepts an
inherited zero interrupt-enable register. Initial DMA-vs-PIO testing remains
polled. The interrupt handler only handles the currently armed DMA events. Eight
consecutive spurious interrupts request masking only the owned GIC SPI and
disable IRQ waits. ISENABLER is read back. A failed mask is counted, the
controller interrupt-enable register is cleared, and a persistent or
unquenchable interrupt storm panics rather than looping indefinitely.
The worker experiment queues only verified read-only windows; writable windows,
writes and flushes retain their original synchronous path. One I/O worker
processes admitted reads in FIFO order. Each read reserves a dedicated
completion thread, starting with one and growing on demand to at most 64;
there are 32 I/O credits. The I/O credit is returned after reading and copying,
while the completion slot and references remain until the callback returns.
A request that cannot allocate its queue record or obtain a credit, completion
slot or new thread uses the original synchronous polled path, rather than returning a new capacity error.
Such a fallback immutable read can overtake queued reads. A stopped device
rejects new requests; stop does not wait for callbacks, aborts unstarted queued
reads, and retains the provider and buffers for in-flight operations. Callback
completion order is not guaranteed. The actual XNU runtime, demand growth cost,
stop boundary and hardware performance require validation. The worker is
compiled out unless `NATIVE_MSDC_WORKER_EXPERIMENT=1` is explicitly supplied
with IRQ wait, block devices and verified root.
The publisher or worker holds the host mutex across sleep; the primary IRQ handler takes
only a separate spin lock and never waits on that mutex. If no IRQ arrives in 10 ms, the driver disables this wait
mode and continues the in-flight DMA through the original polling path and
original one-second deadline. The DMA self-test can still reject DMA and use
PIO. A transfer error does not trigger an unsafe blind PIO retry.

`native_msdc_worker_stats` counts queued, I/O-completed and saturation/thread-start fallback requests; `native_msdc_irq_stats` contains captured IRQs, wait attempts, missing-delivery
fallbacks, and aggregate wait nanoseconds. It does not measure CPU time. These
changes require XNU build/link and real-device qualification; the host tests
model synchronization and are not physical DMA or data-integrity evidence.
Run `python3 tests/irq-wait-host.py`, `python3 tests/irq-dma-host.py`,
`python3 tests/irq-setup-host.py`, `python3 tests/irq-fallback-host.py`,
`python3 tests/irq-caller-policy.py`, and `python3 tests/irq-worker-host.py`.

When the experimental IRQ mode is requested, a retained activation thread waits
up to 60 seconds for `armcpu_spi_boot_ready()` without holding the host mutex.
Until activation, publication and block requests retain the polled path. Once
ready it makes exactly one registration attempt under the host mutex; rejected
metadata, inherited interrupts, allocation failure, or any attach failure is
terminal for this boot. A lifecycle mutex serializes registration and worker
admission with stop. The publisher still starts independently, and the worker
is started only for a valid root with successful IRQ registration. The GIC
readiness API requires a matching ArmCpu build with release/acquire publication.
`python3 tests/irq-activation-host.py` covers delayed readiness, terminal
failure, ownership and stop races with host mocks; it does not validate hardware
IRQ delivery or prove a latency improvement. External SYSIRQ writers that do
not use the ArmCpu shared lock remain an integration assumption.

The publisher has an independently retained cleanup thread, started before
publication is admitted. Stop signals that thread without waiting for it. It
calls `terminate(kIOServiceRequired)` on the parent, retrying failure at most
eight times. Exhaustion sets `native_msdc_publisher_terminate_failed`; the
parent may remain active and teardown has not been proved. XNU termination recursively
makes attached single-provider children inactive; its `startMatching` checks
inactive under arbitration. A publication already committed before stop may
briefly match until this asynchronous teardown takes effect; stop returning is
not a synchronous teardown barrier. The matching guarantee assumes successful
termination and that the child remains a single-provider client of this parent. The host
lifecycle model includes both registration/termination orders, retry and
permanent failure, startup ordering,
abort, wake-before-wait and retained lifetimes; actual IOKit teardown and
hardware execution remain unverified. `python3 tests/publisher-terminate-host.py`
uses the real driver functions with mocked IOKit state, not the XNU workloop.
When stop races an already-entered child start, the publisher also attempts
to terminate that child after start returns; this runs off the stop caller.
`publisher-stop-host.py` tests this retirement and bounded failure using the
actual publisher function. `publisher-child-start-host.py` still reproduces
the registration race in isolation. None is a full IOKit workloop test.

The activation/stop lock order is host mutex, activation mutex, then ArmCpu's
SPI spin lock for attachment; stop sets its cancellation flag first and then
acquires the activation mutex, never the host mutex. ArmCpu's guarded region
contains register operations and table updates, not callbacks or allocations.
Stop can nevertheless wait for that SPI/MMIO operation to finish, so it is not
a proven wait-free operation. Independently, direct stop can race registration
before the asynchronous cleanup makes the parent inactive. Do not interpret a
passing model as establishing either property for arbitrary stop callers.

`native_msdc_stop_while_active` counts stop calls observed while the service is
still active. In the XNU single-provider path `requestTerminate` delegates to
`terminateClient`, which invokes the child's `terminate` and marks it inactive
before the later stop action. A nonzero count is evidence that this expected
lifecycle ordering was not observed; it does not itself repair direct-stop
publication races or guarantee a complete teardown.

Optional `native-msdc-iostat=1` adds cumulative diagnostics, disabled by
default. `native_msdc_transport_stats` contains [DMA read, PIO read, write]
by [<=4 KiB, 4–16 KiB, >16 KiB] and four counters per bin: calls, successful
bytes, failures, and total elapsed nanoseconds including failures. It measures
physical `transfer()` calls, not individual user requests; it excludes time
waiting for the host mutex and time spent in verified-root hashing. DMA time
includes polling or IRQ wait, controller work and the DMA bounce copy. Using
successful-byte deltas divided by the elapsed interval gives an observed
average rate, not a hardware bus bandwidth measurement; dividing by summed
transfer time includes failed calls. Existing PIO self-test is outside this
measurement. The optional block-offer array contains requested calls and bytes
by read/write and size, before validation/completion; it is not a count of
successful requests and contains no per-process identity. Worker queue stats
count executed jobs, cumulative queue wait and maximum queue wait; canceled
jobs and direct-path reads are excluded. Clock reads, conversions and atomic
updates add diagnostic overhead per transfer or queued request. Counters are
individually atomic, not a simultaneous snapshot. This does not measure memory
bandwidth, CPU time, network or GPU utilization, or end-to-end per-request
latency. Tests: `python3 tests/irq-fallback-host.py` and
`python3 tests/irq-worker-host.py`.

### Optional read-submission attribution

`native-msdc-attribution=1` (or the compact `native-msdc-attrib=1`; exactly 1, default off) enables a resident,
one-shot diagnostic reporter after `NativeMsdc::start` has passed all failure
returns. It retains the service until it exits. Stop is monotonic, disables
recording, and the reporter exits after at most its next ten-second sleep;
the final partial interval is not emitted. The built-in archive is not an
unloadable diagnostic kext and no restart after stop is supported.

The root hook runs in `doAsyncReadWrite` **before** BlockJob allocation or
queueing. It records only eligible read offers with the same In direction as
the existing driver, valid extent and adequate descriptor length. InOut is
unsupported by the driver and is counted as a skipped direction, not changed
into a read. Null/zero, nonread direction, extent and short-descriptor skips
have separate counters. Data-overlay reads are recorded separately at
`storeTransfer` after its range validation and caller admission guard.

Rows group submit execution PID, thread ID, root-window index, attribute flags
and size bin (0 <=4KiB, 1 <=16KiB, 2 larger). Window legend lines identify the
manifest windows; UINT32_MAX denotes the persistent-store backend. PID/TID are
**execution context**, not proven origin, file owner or file identity. PID0
must never be described as the application that requested the file. No process
or vnode pointer/name is retained, looked up or printed.

Flags are 1 has-bufattr, 2 metadata, 4 passive, 8 nocache, 16 static-content,
32 direct-store. A missing bufattr is unknown, not data. These are real
`IOStorageAttributes::bufattr` values supplied by IOMediaBSDClient, inspected
synchronously while that call owns them. Private XNU KPI readers are used;
there is no manual bufattr layout access. The 32-byte attributes size, bufattr
offset24 and static-content option0x20 are checked against the pinned
IOStorageFamily public header. Metadata/passive/static flags do **not** prove
VM page-in versus syscall read, or identify a specific pathname.

The fixed 128-row table uses a nonblocking trylock and at most eight probes.
Ten-second snapshots reset the table, sort outside its lock and print top12
rows plus `other` totals. Each row has a separate span record, joined by sequence
and rank; full-width numeric records are <=240 bytes. Overflow and contention counters are explicit;
contention calls/bytes and skipped counters are independently sampled pairs.
First/last block mean first/last **submitted starting block**, not a min/max
extent or a repeat detector. Root units are512 bytes; store units are4096.
No allocations, content reads, console output, new waits, VM lookups or
completion lifetime changes occur on the recording path. The reporter's
snapshot buffer is static, not a large kernel-stack object.

This is offered traffic after diagnostic activation, not physical DMA,
completion bytes, cache-hit count or a census of startup policy-region reads.
Compare **interval deltas** with existing `native-msdc-iostat=1`, not lifetime
byte equality. Signed verification block rounding, cache hits, policy reads
and I/O failures can separate the two totals. Empty intervals and all losses
remain visible. Diagnostic output is not a throughput qualification.

Host test: `python3 tests/check-read-attribution.py` (C++17 ASan/UBSan).
It executes the actual KPI hook, reporter formatter/top12/remainder/exit,
thread-start failure and owner stop checks, plus concurrent table accounting,
InOut rejection and block-device-off helper compilation. TSan may need a
supported macOS/Linux host; a runtime initialization failure is not a pass.

Before deployment, compile/prelink the real iOS kernel target. This source
requires `<sys/proc.h>`, `<sys/buf.h>` with the existing KERNEL_PRIVATE compile
context, and the core exports `proc_selfpid`, `thread_tid`, `bufattr_meta`,
`bufattr_passive`, `bufattr_nocache`. No fake declarations or guessed ABI are
permitted. The existing build uses public IOStorageFamily revision
`e45fbe30a7879e0b16e26dddb78538f1b00e0bff` via prepare-storage-headers.py.
Incremental owner build (preserving the active IRQ/worker/verified profile):

```
XNU_NATIVE_MSDC_BLOCK=1 XNU_MSDC_IRQ_WAIT=1 XNU_MSDC_IRQ_WORKER=1 \
  python3 scripts/build-arm-driver.py NativeMsdc --verified-root
```

Rebuild storage bindings/prelink through the existing owner build afterward;
no GPU/Mesa/Rust rebuild is required. Actual SDK/link and device results must
be recorded separately from host fixture success.

For VerifiedRoot CPU cost, the existing independent `native-root-timing=1`
selector enables read/vr_verify_block wall counters even without a new XNU
ROOT_TIMING build. Preserve that distinction from eager native4k signature
preparation timing, which requires XNU_ROOT_TIMING compiled into BSD.

The compact selector is20 characters, exactly the same length as
`native-msdc-iostat=1`. A profile at the1023-byte limit can replace that
diagnostic selector without deleting functional boot arguments. That switches
away from iostat/CPU-load fallback reporting and is a diagnostic-profile change,
not a strict single-variable performance comparison. No automatic replacement
or boot image editing is performed by the driver.

### Bounded overlap trace

`native-io-trace=1` enables the same resident reporter without requiring
`native-msdc-attrib=1`. Only In offers passing the initial direction, extent
and descriptor-length checks from verified, manifest-read-only, non-writable
windows are sampled before worker queueing. The trace excludes the persistent
store, writable windows, completions, and physical reads. Later queue, completion, online, verification and media checks
may reject an offer. The window legend maps index to name.

Each 10-second drain emits at most 32 event records (the first 32 admitted in
that interval), with PID/TID of the submission context, boot-relative
`mach_absolute_time` in ns, window-relative block/count in 512-byte sectors.
`full` counts offers dropped after observing saturation; `contention` counts
try-lock failures. After saturation, timestamps and identities are not fetched.
Both counters are exact across quiescent drains, but sampled independently; an
offer racing a drain can be counted in an adjacent interval. Drain failure
retains the buffer for a later interval. A stopped reporter discards its final
partial interval. Empty reports are emitted, and reporting itself may perturb
timing. Never interpret absent overlap as proof of no overlap when data is
sampled or lost. This diagnostic exposes block ranges, which can identify data
on a known image; it never emits data bytes or process names.
