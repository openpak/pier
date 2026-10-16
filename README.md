# Pier

Pier is OpenPak's PlayStation 3 emulator, built on the RPCS3 codebase.
It is an independent project with its own name, direction and OpenPak
integration. The goal is to run the console's own firmware (XMB) against
OpenPak's replacement online services, the way Harbor does for the Switch.

**RPCS3 remains our upstream base.** We acknowledge and preserve the work of
RPCS3 and its contributors, and integrate applicable upstream improvements.
Pier is maintained by OpenPak; it is not an official RPCS3 release and is not
presented as endorsed by the RPCS3 project. The original README is kept in
[README.upstream.md](README.upstream.md).

## Status

Firmware 4.93 boots and native OpenPak account/profile integration has been
exercised. Native game networking is incomplete: firmware services and IPC must
survive game launch. The old single-process replacement launch is disabled by
default; `PIER_ALLOW_OFFLINE_HANDOFF=1` enables it only for offline diagnostics.
With the native console descriptor (Linux), a VSH game launch instead starts the
game as a separate Pier host while VSH keeps running; this path is not yet
accepted (see `docs/ps3/native-console-processes.md` in the OpenPak docs repo).
OpenPak does not use an emulator-only RPCN game bridge. Firmware and physical
PS3 clients must use the same native PS3 endpoints, backed by account core.
Plan: `docs/ps3/native-service-architecture.md` in the OpenPak docs repo.
Build only through the shared OpenPak build lock.

## Licence

GPL-2.0-only for the forked tree (most RPCS3 files; some files carry other
licences in their headers, which are preserved).
