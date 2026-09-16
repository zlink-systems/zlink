[English](./framework-cpp-0.16.0.md) | [한국어](./framework-cpp-0.16.0.ko.md)

# ZLink C++ Framework 0.16.0 release notes

Framework 0.16.0 uses binding 1.2.0 and Core 1.2.0. Each framework language is versioned independently.

## Contract change

- A select-one channel left with no member after eligibility and drain now ends as `Unavailable`, and a request and a one-way send agree. A member dropped because its weight is `0` or because it is draining falls here; the send path and the connection are still there, so it is not `NotFound`. The languages used to answer differently.
- A channel send used to end as `not_found` here while a request ended as unavailable; the two now agree.

## Fixes

- A delay timer still pending when the process exits no longer reads a registry destroyed by static destruction. It surfaced when a request-expiry wait outlived main.

## Installation

Download `zlink-framework-cpp-0.16.0.tar.gz` from the [`framework-cpp/v0.16.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.16.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. A vcpkg overlay port and a Conan recipe are also available.
