# Mbed TLS

The build extracts the official Mbed TLS 4.1.1 source archive from
`mbedtls-4.1.1.tar.bz2`. Its published SHA-256 is
`3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c`.

The archive contains the Mbed TLS and TF-PSA-Crypto source trees and their
Apache-2.0 / GPL-2.0-or-later license files. See the upstream release notes at
<https://github.com/Mbed-TLS/mbedtls/releases>.

The root Makefile extracts this pinned archive under `build/` and uses its
CMake targets to build the static TLS, X.509, and PSA Crypto libraries for the
freestanding x86-64 userland. The target-specific allocator, RTC clock, and
external kernel CSPRNG hooks are configured in `userland/lib/tls/`.
