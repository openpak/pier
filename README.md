# Pier

Pier is OpenPak's PlayStation 3 emulator, built on [RPCS3](https://github.com/RPCS3/rpcs3).
It runs the console's own firmware (the XMB) and connects it to OpenPak's
replacement online services, in the same way Harbor does for the Switch.

Pier is an independent project with its own name and direction. RPCS3 remains
our upstream base: we preserve its work and credit, and integrate applicable
upstream improvements. Pier is not an official RPCS3 release.

**Status:** Experimental (boots, online incomplete)

## Not affiliated

Pier is an independent project and is not endorsed by the RPCS3 project. It is
not affiliated with Sony Interactive Entertainment. PlayStation and related
names are trademarks of their respective owners.

## What works today

- Builds on Linux and boots PS3 firmware 4.93 to the XMB.
- The firmware's own PSN sign-in form talks to OpenPak's PS3 account service,
  which is backed by the shared OpenPak account core. A native sign-in with an
  OpenPak account has been completed, and profile and avatar reads were observed.
- XMPP presence (the friends stream) connects, stays up past the old idle
  disconnect, and reports friends' online status across OpenPak platforms. A
  populated Friends screen has not been confirmed visually yet.
- A game installed on the emulated HDD can be started from the XMB. In one test
  the title's code ran and exited; whether the menu or gameplay is reachable is
  not verified.

Not verified: any online game session (hosting, joining, matchmaking), trophy
sync from firmware or a game, accounts with two-factor authentication, and any
run against a real PS3 for comparison.

## Connecting to OpenPak

Pier hands the emulated firmware OpenPak's DNS by default. The PS3 service
names then resolve to OpenPak, and the firmware signs in with the normal XMB
form using an OpenPak account email and password. There is no separate Pier
login dialog yet. The emulated firmware must trust OpenPak's network CA, which
today means adding the CA to its certificate store and patching the firmware's
certificate pin check in the emulated code; this setup is still a manual,
OpenPak-internal step.

You supply the PS3 firmware and any games yourself, from a console you own.
OpenPak provides none and never will. Create an account at https://openpak.org.

## Downloads

No public build yet. The version tags in this repository come from RPCS3 and are
not Pier releases. Releases will appear at https://github.com/openpak/pier/releases.

## Building from source

Follow [BUILDING.md](BUILDING.md), inherited from RPCS3. On Linux you need
Clang 17+ or GCC 13+, CMake 3.28+, Qt 6.12, the Vulkan SDK and SDL3. Pier stores its settings and firmware under
`~/.config/pier` and its caches under `~/.cache/pier` (per-user folders on macOS
and Windows; a `portable/` folder next to the binary takes precedence).

## Known limitations

- Game networking is incomplete. Firmware services and IPC must survive a game
  launch, and that path is not yet accepted. The old single-process replacement
  launch is disabled by default; `PIER_ALLOW_OFFLINE_HANDOFF=1` enables it for
  offline diagnostics only. With the shared native console (the default on
  Linux, `PIER_NATIVE_CONSOLE=0` opts out), a game launched from the XMB runs as
  a separate Pier process while the XMB keeps running.
- OpenPak does not use an emulator-only game bridge: Pier and a real PS3 are
  meant to use the same native PS3 endpoints.
- The trophy backend exists, but a trophy sync has not been seen end to end.
- Only Linux has been exercised. Android and other platforms are not available.
- Everything in the RPCS3 compatibility story still applies: many games do not
  run, and Pier does not claim otherwise.

## Contributing and support

Report bugs and ask questions at https://github.com/openpak/pier/issues. Do not
file Pier problems with the RPCS3 project. To report a security problem, contact
the OpenPak maintainers privately through the contact on https://openpak.org
and do not open a public issue. Most useful: testing the firmware sign-in and
friends flow with your own console files, and capturing logs of game launches
from the XMB. Coding style and developer notes are in the upstream RPCS3 wiki.

## License and acknowledgements

GPL-2.0-only for the forked tree (see [LICENSE](LICENSE)). Most files are RPCS3's;
some carry other licences in their headers, which are preserved. Pier exists
because of the RPCS3 developers and contributors; thank you.

The original RPCS3 README is kept in [README.upstream.md](README.upstream.md).
