# Notes: pier

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

## Known limitations

- Game networking is incomplete. Firmware services and IPC must survive a game
  launch, and that path is not yet accepted. On Linux a game launched from the
  XMB always runs as a separate Pier process on a shared native console while
  the XMB keeps running. Other platforms have no native console yet: there the
  only launch replaces the XMB with the game, is disabled by default, and
  `PIER_ALLOW_OFFLINE_HANDOFF=1` enables it for offline diagnostics only.
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

GPL-2.0-only for the forked tree (see [LICENSE](../LICENSE)). Most files are RPCS3's;
some carry other licences in their headers, which are preserved. Pier exists
because of the RPCS3 developers and contributors; thank you.

The original RPCS3 README is kept in [README.upstream.md](../README.upstream.md).
