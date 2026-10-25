# Evidence: pier

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
