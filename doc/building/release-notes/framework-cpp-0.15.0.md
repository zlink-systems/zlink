[English](./framework-cpp-0.15.0.md) | [한국어](./framework-cpp-0.15.0.ko.md)

# ZLink C++ Framework 0.15.0 release notes

Framework 0.15.0 uses binding 1.2.0 and Core 1.2.0. Each framework language is versioned independently.

## Changes

- The Classic fanout publisher takes a `no_drop` setting. With it on, a record reaches every pipe whose topic matches or none of them, and a record that cannot be sent ends as `DeadlineExceeded`.
- A fanout subscriber registers the topics it receives at startup. It receives the byte-prefix union of the registered topics; with no registration it receives only the empty topic.
- Fanout publish waits for local admission before returning. Waiting past the send timeout ends as `DeadlineExceeded`.
- Owner liveness is judged where a descriptor is first admitted. A descriptor whose owner lease has ended is rejected at admission.
- Omitting `advertise_host` on a wildcard bind host advertises the loopback of the same address family: `127.0.0.1` for `0.0.0.0`, `::1` for `::`. Set it only when other hosts must connect.
- Changing a channel weight at runtime now sends a descriptor update to the peers. A channel served by a single node kept accepting calls after its weight was set to `0`; it no longer does.
- `app::run` reports startup validation failures.

## Installation

Download `zlink-framework-cpp-0.15.0.tar.gz` from the [`framework-cpp/v0.15.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.15.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. A vcpkg overlay port and a Conan recipe are also available.
