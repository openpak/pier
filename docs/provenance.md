# Pier provenance

What Pier does today and where its code comes from.

## Base

Pier is a fork of [RPCS3](https://github.com/RPCS3/rpcs3), taken from RPCS3
`master` at commit `ffba2bf09` (2026-10-13). Every file not listed below is
RPCS3's, unchanged, under its original licence (GPL-2.0-only for the tree; some
files carry other licences in their headers). The original README is kept as
`README.upstream.md`.

The changes listed below are OpenPak's own work, written for Pier. Where they
follow firmware behaviour (call sites, argument layouts, event numbers, marker
values), that behaviour was observed from the running firmware and written down
as layout facts; no firmware or decompiled code is included. Users supply their
own firmware and games.

## What Pier does

Pier boots the PS3 firmware (4.93, `vsh.self`) to the XMB and connects it to
OpenPak's services the way a real console on OpenPak's DNS would: the
firmware's own sign-in, presence and service requests go to native PS3
endpoints. Pier has no emulator-only game bridge and rewrites no host names
itself.

On Linux every firmware boot starts a shared native console. A game launched
from the XMB runs as a separate Pier process (a child host) attached to that
console, while the XMB and its firmware services keep running. Windows and
macOS have no native console yet. There the only launch replaces the XMB with
the game, and it is disabled unless `PIER_ALLOW_OFFLINE_HANDOFF=1` is set, for
offline diagnostics only.

## OpenPak changes by area

### Native console (Linux)

Shared state lets several Pier processes act as one console:

- `rpcs3/Emu/Cell/lv2/native_console_memory.h`,
  `native_console_control.h`, `native_console_sync.h`,
  `native_console_runtime.h`, `native_console_child.h`. Descriptor-backed
  console memory, shared keyed mutexes, conditions, PPU event queues and IPC
  ports, guest process IDs and parents, globally unique PPU IDs, and an owned
  child supervisor (spawn, wait, reap, parent-death cleanup). A child receives
  the console through an inherited descriptor and a one-use guest token.
- `sys_mutex`, `sys_cond`, `sys_event`, `sys_mmapper`, `sys_ppu_thread`,
  `PPUThread.cpp`, `IdManager.cpp`, `util/vm.hpp`, `util/vm_native.cpp`. LV2
  objects with an IPC key, the SYSUTIL and MIO shared memory regions, and PPU
  IDs use that shared state. Without a native console, upstream behaviour is
  unchanged.
- `sys_process.cpp`, `sys_process_spawn.h`. The XMB starts a game with
  `sys_process_spawns_a_self2` (priority 1001, a block of big-endian 64-bit
  pointers: path and arguments, NULL, environment, NULL, with all strings inside
  the block). Pier decodes it and starts the child host with VSH's exact argv
  and envp. `sys_process_wait_for_child2` gives VSH the child's exit. Inside a
  child, exitspawn is refused. `pier::is_native_game_child()` tells a child
  host apart from the firmware host.
- `System.cpp`, `rpcs3.cpp`, `rpcs3qt/gs_frame.cpp`. The child host uses the
  firmware's libsysutil, NP, NP2 and netctl libraries (LLE). It shares the
  firmware's caches with its own lock and log, and exits as soon as its guest
  stops. On X11 it draws inside the firmware's window, and F8 quits back to the
  XMB.
- `sys_net/nt_p2p_port.cpp`. A child host binds an ephemeral host port when VSH
  already holds the P2P port.

### Firmware boot

- `System.cpp`. A firmware boot uses one PPU thread slot (VSH 4.93 has a lock
  order between its main thread and the HDMI handler that deadlocks with two
  concurrent slots). An inserted disc does not rewrite firmware executable
  paths.
- `PPUThread.cpp`. Once the firmware cache exists, the per-boot precompile sweep
  of `/dev_flash/sys/external` is skipped.
- `sys_ppu_thread.cpp`. VSH's libmtp `CellMtpIpLauncher` worker is created
  joinable, because the firmware joins it before unloading the module.
- `sys_fs.cpp`. `sys_fs_truncate2` is implemented (VSH's media database sizes
  its journal with it).
- `RSX/VK/VKGSRender.cpp`. The texture cache is released before the render
  targets it references.
- `Utilities/File.cpp`. Pier keeps its profile in per-user folders named after
  Pier (`~/.config/pier` and `~/.cache/pier` on Linux, the platform
  equivalents elsewhere). A `portable/` folder next to the binary still takes
  precedence.

### Network and PSN

- `sys_net.cpp`, `sys_net.h`, `sys_net/sys_net_helpers.*`,
  `sys_net/lv2_socket_native.*`. An emulated wired port (`eth0`) for the
  firmware's network manager: link state and link events, interface flags and
  addresses, the interface list, and a DHCP server answering the firmware's
  DHCP client from the host's address, netmask, gateway and DNS. The DHCP
  client's socket options and port are adapted for the host.
- `NP/np_handler.cpp`, `system_config.h`. The console reports a wired link
  whenever the host has a route. Without a readable host MAC, one is derived
  from the PSID. The defaults are: internet enabled, DNS set to OpenPak's
  resolver, windowed start.
- `sys_ss.cpp`. The secure-module call used by the firmware's sign-in returns
  the marker the firmware checks for. OpenPak's server does not depend on the
  remaining bytes.
- `Modules/sceNp2.*`, `NP/np_requests.cpp`, `NP/server_list.h`. Matching2
  fixes: unused server-ID slots are zeroed, a failed world-list request
  reports an error callback instead of aborting, the user-info layout matches
  the SDK (64-byte entries, joined-session pointer at offset 56), and the
  unimplemented `GetUserInfoList` reports unavailable.

### Tests, build and docs

- `scripts/tests/pier_*.cpp`. Standalone regressions for the spawn-argument
  codec, server-list copy and the native console memory, sync, events and
  processes, built with a plain C++20 compiler outside the emulator build.
- `.github/workflows/nightly.yml`. A rolling `nightly` pre-release, built only
  when the source changed.
- `README.md`, `docs/provenance.md`. Pier's documentation.
