# Deploy: pier

## Downloads

No public build yet. The version tags in this repository come from RPCS3 and are
not Pier releases. Releases will appear at https://github.com/openpak/pier/releases.

## Building from source

Follow [BUILDING.md](../BUILDING.md), inherited from RPCS3. On Linux you need
Clang 17+ or GCC 13+, CMake 3.28+, Qt 6.12, the Vulkan SDK and SDL3. Pier stores its settings and firmware under
`~/.config/pier` and its caches under `~/.cache/pier` (per-user folders on macOS
and Windows; a `portable/` folder next to the binary takes precedence).
