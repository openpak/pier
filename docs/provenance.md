> Architecture correction, 2026-10-15: historical HLE/RPCN bridge, private
> user lookup, ticket export and bridge login work recorded below has been
> removed from active sources. Native firmware/real-console service parity is
> required. The single-process VSH replacement is offline-diagnostic-only.
> Native account/profile endpoints and private trophy storage are retained.

# Pier implementation facts

## XMB installed-game handoff (2026-10-15)

Observed locally with firmware 4.93 and an HDD test copy of Terraria BLES01938:
XMB calls syscall 27 (`sys_process_spawns_a_self2`) with priority 1001, a
0x50-byte argument block, and then queries the returned process ID and waits
for the child. The upstream implementation returns success without creating
one. The later UI error is 80010002; the log alone does not prove which of the
subsequent failures supplies that UI error.

Argument-block format was established from the firmware's caller at
0x00608d10 and packing routines at 0x00608950/0x00608834: big-endian 64-bit
pointers; executable path followed by arguments, NULL, environment strings,
NULL; strings are inside the supplied block. These are layout facts. The
parser and test fixtures are independently written; no firmware code is
included in Pier.

The current compatibility path handles only a VSH caller, observed priority
1001, and `/dev_hdd0/game/.../USRDIR/EBOOT.BIN`. It replaces the single emulated
process with the selected executable via Pier's existing main-thread boot
and continuous-window lifecycle. It does not implement concurrent VSH and
game processes, return to XMB after game exit, or arbitrary child processes.
Other spawn requests retain upstream behavior. Live game startup remains
unverified until the user retries with this build.

Validation: standalone argument parser regression compiled with C++20,
AddressSanitizer and UndefinedBehaviorSanitizer. It covers path/argument/env
roundtrip, observed 0x50-byte layout, truncated lists, missing string/list
terminators, out-of-block pointers, 64-bit pointer overflow, and empty argv.

## VSH libmtp shutdown compatibility (2026-10-15)

Firmware 4.93 creates `CellMtpIpLauncher` at priority 3068 with flags 0
(detached). Its stop sequence posts exit event 9, calls thread join, then
destroys the event queue and unloads libmtp. In the captured failure, join
returned CELL_EINVAL, libmtp was unloaded, and the worker resumed at an
unmapped instruction immediately after event receive. This is a firmware
module lifetime race, before confirmed Terraria guest execution.

Pier makes only this named worker joinable when created by VSH with the
observed priority and detached flags. The stop routine can then wait for its
exit before unloading code. This is a narrow compatibility workaround, not a
change to general LV2 detached-thread semantics. No firmware code is copied.
Live shutdown and game handoff validation are required independently.

## Vulkan shutdown resource order (2026-10-15)

The patched VSH run reached the Terraria HDD handoff, then failed on the host
main thread in `__dynamic_cast`, called from texture-cache storage clearing
during `VKGSRender` destruction. Framebuffer-backed texture sections call
`get_render_target()->on_unlock()` while being unprotected. The destructor
previously destroyed the render-target store first, leaving those pointers
dangling. Destroy the texture cache before its referenced render targets.
This change addresses the captured shutdown lifetime violation; game startup
must be validated separately.

Live validation passed renderer teardown twice: the database-rebuild reboot
and the Terraria HDD handoff. Terraria's own main thread then executed, but
exited with status 0 after asset reads used `/dev_hdd0/game/module/USRDIR/`
and trophy reads used VSH's directory. The handoff had supplied argv[0], which
skips Load's directory derivation. Clear only argv[0] before loading the
validated executable so Load regenerates the same executable path and the
new title's directory; preserve the other arguments and environment.

## Authenticated HLE game session transport (2026-10-15)

When explicitly opted in through `PIER_OPENPAK_SESSION`, games load an owner-local
session configuration exported by np-account after native firmware authentication.
The transport contains a signed, expiring ticket and an explicit loopback backend,
not the user's core password. VSH keeps its native authentication path. HLE uses
RPCN's existing matching2 implementation against the OpenPak fork's private
listener, which verifies the credential against current core state. No simulated
login is enabled. Missing/invalid transport remains offline, and backend login
failure clears HLE online state. The public RPCN server is not used in this mode.

This is a local emulator bridge; it does not implement concurrent VSH processes,
real-hardware native matchmaking, or prove game-to-game P2P compatibility.

## Local backend protocol version (2026-10-15)

Live Terraria trace reached TLS but rejected backend greeting33 against Pier
expected32, before login. Upstream RPCN commit253b3fe changes32->33 and appends
GetTss after existing commands; existing login and matching payloads are
unchanged. Updated Pier's expected version to33, without changing backend
advertisement. Online black-box regression now optionally reads Pier's expected
version from PIER_SOURCE and compares the real TLS greeting before login.
The current real firmware ticket was also accepted by the local backend with
login status0 in a one-shot live probe (no ticket or password printed). This
proves ticket/backend authentication, not actual Terraria room creation.

## Terraria server-list capacity and world error (2026-10-15)

After real-avatar projection, live game login/ticket/server discovery succeeded.
GetServerIdListLocal was called with capacity4; backend returned only server1.
Terraria scans all4 slots, skips zero IDs, and ignores the returned actual count.
Observed from the user backup ELF at1b1b0..1b500 (PT_LOAD file offset=VA-0x10000):
helper16068 returns4, and loop checks each BE16 slot for zero. Unused slots
previously retained guest stack bytes, producing invalid server53267.
Backend returned DbFail; reply_get_world_list asserted instead of queuing an
error and froze emulation. No host core was produced; Pier remained alive.

Compatibility policy: write real IDs then zero unused caller capacity, without
changing actual count or creating synthetic servers. Validate writable output
size. World-list backend failures now queue a standard Matching2 error callback
(forbidden for Unauthorized; internal-server error otherwise), rather than a
fatal assertion. Malformed success replies retain existing parser validation.
Standalone ASan/UBSan regression covers one-server/four-slot case, truncation,
empty input, zero capacity and preservation beyond caller capacity.

## Matching2 user-list response layout (2026-10-15)

Terraria callback at guest 0x1a610 reads a joined-session pointer at user
entry offset 56; 0x1a718 reads its count at offset 60, and 0x1a74c advances
entries by 64 bytes. Sessions have 32-byte stride (0x1a60c). Corrected the
previously unused inline session declaration to a guest pointer, allocate
actual sessions in callback event memory, and assert entry offsets/size.
These are observed binary-layout facts, not copied firmware/game code.

## User lookup reply routing (2026-10-15)

Live online retry received backend success for OpenPakGetUserInfoList (512),
but the transport forwarded it to the asynchronous NP handler, whose unknown
command branch threw and froze emulation. The caller uses forge_send_reply
and waits on replies_sync. Added this private command to the synchronous
reply dispatcher alongside GetServerList. Backend success alone does not
validate the client receive path or the game callback.

## Nonblocking user lookup (2026-10-15)

After reply routing correction, live logs show repeated successful lookup
callbacks, with no fatal error. Terraria also calls GetUserInfoList from its
main thread; blocking forge_send_reply delayed each callback roughly1.5s.
Changed to queued forge_send and an explicit asynchronous NP reply handler,
with request-owned copies of IDs/options and a checked response decoder.
GetServerIdListLocal also repeatedly fetched over transport (~0.7s); private
OpenPak server IDs are now cached only within the matching context, and
failed fetches are not cached. Context destruction discards the cache.
Actual responsiveness and hosting require live retry after deployment.

## Native IPC producer trace and diagnostic launch (2026-10-15)

The bridge entries above are historical; architecture-correction-audit.md in
OpenPak docs records their removal. Native firmware/game IPC is required.
An isolated, disconnected VSH gametool-mode probe created the real keyed
packet memory/mutexes/conditions and started Matching2 workers. Details and
acceptance boundaries: docs/ps3/native-ipc-lifecycle.md in OpenPak docs.
No firmware implementation or decompiled source is incorporated here.

Fixed the executable-after-`--` CLI path: resolve the already selected spath,
which may come from emu_argv, instead of indexing an empty positional list.
Shared mutex diagnostics include flags/name to distinguish native producer
creation from consumer open attempts. No launch mode default or guest IPC
semantics changed. Firmware/game concurrent execution is still unfinished.

## Native console system-memory foundation (2026-10-15)

Added Linux descriptor-backed console system memory for the existing LV2
SYSUTIL and MIO regions. Version/extent/seal validation and CLOEXEC descriptor
duplication provide an explicit console capability; separate host address
spaces can map the same backing without a named file or service. Both shared
memory mapping syscalls accept this backing only under the diagnostic inherited
PIER_NATIVE_CONSOLE_FD capability. Normal memory paths use offset zero.

The standalone exec-separated regression passes ASan/UBSan (shared writes,
console isolation, invalid attachment, resize rejection and mapping lifetime).
A rebuilt Pier firmware probe wrote both native packet headers into this
backing, observed from a separate supervisor process; the helper wrote none of
those headers itself. All implementation is original host compatibility code;
no firmware or decompiled implementation is incorporated.

This completes the storage boundary only. Native shared mutex/condition/event
objects, guest process identity/lifecycle, save-state/reboot integration and
native online gameplay remain unimplemented. Normal launcher defaults and the
VSH-destructive offline handoff guard remain as before. See OpenPak docs
ps3/native-console-memory.md for evidence and limitations.

## Native console mutex/condition foundation (2026-10-15)

Original Linux host compatibility code now supplies console-scoped keyed mutexes
and conditions behind the explicit inherited descriptor. The ABI-two control
arena uses robust process-shared metadata locking and shared futex wakeups,
indices/generations and numeric actors rather than host pointers. Create/open,
recursive ownership, FIFO/enqueue priority, timeout reacquisition, cancellation,
broadcast, reference lifetime and peer death are covered by separate-exec
ASan/UBSan regression tests. Metadata owner death fails the domain closed.
No firmware code or decompiled implementation is incorporated.

Full Pier build passed. An isolated real VSH probe created both channel mutexes
and eight conditions; a separate supervisor opened them and acquired both locks
while reading the real packet headers. Default no-descriptor firmware startup
also passed. Evidence under OpenPak scratch/pier/native-network:
console-sync-build-retry.log, console-sync-sanitizers.log,
console-sync-firmware-observer.log, console-sync-firmware-evidence.log and
console-sync-default-regression-state.log. Both probes stopped; no controls sent.

Shared events, public guest process/thread identity and child lifecycle remain
unfinished. Priority inheritance and guest signal_to are unsupported; queue
priority is a snapshot. Descriptor-mode native savestate restoration is rejected.
No game request/reply, native hosting or joining acceptance is claimed. Normal
launcher and destructive-handoff guard are unchanged. See OpenPak docs
ps3/native-console-synchronization.md for the current boundary.

## Native shared PPU event queues (2026-10-15)

Added original host compatibility code for keyed PPU event queues and IPC ports
in Linux descriptor mode. The ABI-three host arena stores bounded event payloads,
receiver ordering, generations and peer references. Separate-exec ASan/UBSan
coverage passes FIFO/priority delivery, capacity, timeout, cancellation, forced
destroy/recreate, creator/receiver death and console isolation. Existing memory
and synchronization regressions pass with the enlarged arena.

Live probes exposed missing LV2 scheduler sleep around host waits. Corrected
shared event receive, mutex lock and condition wait to release/reacquire scheduler
slots. The rebuilt VSH observer then opened the real keyed event queue and native
channel synchronization objects from another host, acquiring both locks and
reading firmware-initialized headers without submitting events. Build evidence:
console-events-build-scheduler-complete.log; live evidence:
console-events-firmware-observer-scheduler.log and console-events-firmware-evidence.log
under OpenPak scratch/pier/native-network. No firmware implementation is copied.

Unnamed cross-host event sources and shared SPU queues remain unsupported.
Public guest identity, child lifecycle and real game IPC remain unfinished. No
native online/game-launch acceptance or normal launcher change is claimed. See
OpenPak docs ps3/native-console-events.md for retained initial failure evidence
and the exact boundary.

Default no-descriptor firmware producer/Matching2 regression also passed:
console-events-default-regression-state.log. Both isolated probes stopped; no
control input was sent.

## Shared guest process/thread identity and owned child helper (2026-10-15)

Original Linux host compatibility code adds real console-scoped guest PID/parent
registration, one-use random child capabilities, explicit guest-exit versus host
failure, terminal reaping and orphan cleanup. PPU IDs retain class/local index
encoding and use shared nonwrapping generation allocation, with ownership records
and cleanup. Native targeted condition signals resolve actual actors; unnamed
cross-host event sources now encode registered guest PIDs. Default no-descriptor
behavior remains unchanged. Descriptor/registry ABI is four.

An owned posix_spawn/waitpid helper handles capability inheritance, failed spawn,
child termination and parent-death setup. Separate-exec ASan/UBSan tests cover
normal/negative guest exit, host crash/stop, PID/PPU identity, targeted wakeup,
nested-parent death, capacity/reap, cleanup and isolation. No firmware source or
decompiled implementation is incorporated. This helper does not yet dispatch VSH
game-spawn syscalls or implement renderer/input and firmware exit notifications.
No game boot or native multiplayer acceptance is claimed. See OpenPak docs
ps3/native-console-processes.md for evidence and remaining boundaries.

Final identity/supervision acceptance (2026-10-16): full shared-lock build passed
console-process-slot-order-build.log. Two consecutive VSH observer boots resolved
the real firmware PID/main PPU and opened shared events/channel synchronization,
acquiring both locks and reading firmware packet headers. Evidence:
console-process-slot-order-observer.log and console-process-slot-order-repeat-observer.log,
with corresponding firmware -evidence.log copies. Default no-descriptor producer/
Matching2 startup passed console-process-default-regression-state.log.

Earlier descriptor probes stalled twice; retained logs and correct PID/mode
readings are documented in native-console-processes.md. Corrected native PPU slot
selection to preserve upstream append-first order, then the two consecutive boots
passed. A precise firmware timing root cause is not established. Targeted signal_to
also preserves ESRCH versus EPERM for missing versus nonwaiting threads. All probes
stopped, no controls sent, no normal runtime restarted. Game dispatch remains guarded.
