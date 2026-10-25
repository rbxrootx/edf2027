# libsodium build dependency

The Windows x64 import library and runtime DLL are from the official
`libsodium-1.0.22-stable-msvc.zip` release, built for Visual Studio toolset
v143. The archive SHA-256 was
`2AEAC7E7F00BC0D980096EA8B908E66DFAD5C858D08F1CA3B980D413833303D9`.

Upstream release page: <https://download.libsodium.org/libsodium/releases/>
Upstream license: [../../LICENSES/libsodium.txt](../../LICENSES/libsodium.txt)

Windows builds use the bundled headers, import library, and `libsodium.dll`.
Linux server builds use the system libsodium development package; Docker builds
install that dependency in the image build.
