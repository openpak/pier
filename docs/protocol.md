# Protocol: pier

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
