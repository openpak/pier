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

Phase 0: fork created, nothing OpenPak-specific built or booted yet.
Plan and gate 1: `docs/ps3/pier-base-plan.md` in the OpenPak docs repo.
Build only through the shared OpenPak build lock.

## Licence

GPL-2.0-only for the forked tree (most RPCS3 files; some files carry other
licences in their headers, which are preserved).
