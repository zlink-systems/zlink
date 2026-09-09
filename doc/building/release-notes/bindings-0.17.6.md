[English](./bindings-0.17.6.md) | [한국어](./bindings-0.17.6.ko.md)

# ZLink bindings 0.17.6 release notes

Core stays at 0.17.5; only the binding package version moves to 0.17.6. The four language bindings
share one `BINDINGS_VERSION`, so C++, Java, and .NET are republished under the same number to ship
a single Node.js fix. The public API and behavior of the C++, Java, and .NET bindings are unchanged
from 0.17.5.

## Highlights

- Node.js: the backpressure check hard-coded EAGAIN to the Linux value (11), so on macOS
  (EAGAIN=35) every backpressured send turned into a
  `backpressured send did not return an EAGAIN wait token` error. It now uses the platform's
  `os.constants.errno.EAGAIN`. A `platform_backpressure` regression test was added.
- Node.js: the native addon source clashed with MSVC's `max` macro and failed to build from source on
  Windows arm64; `std::numeric_limits<size_t>::max` is now parenthesized. Platforms without a
  prebuild compile the addon at install time against the Core release archive named by
  `ZLINK_CORE_SOURCE=release` and `ZLINK_CORE_PACKAGE_PREFIX`.

## Installation

| Channel | Package | Version |
| --- | --- | --- |
| npm | `@zlink-systems/zlink` | 0.17.6 |
| nuget.org | `Zlink` | 0.17.6 |
| Maven Central | `systems.zlink:zlink`, `systems.zlink:zlink-ext-netty` | 0.17.6 |
| GitHub Release | `cpp/v0.17.6` source archive | 0.17.6 |

Supported platforms are linux-x64, linux-arm64, macos-arm64, windows-x64, and windows-arm64. Intel
Mac is unsupported from Core up. The prebuilt Linux runtime needs glibc 2.38 or newer.
