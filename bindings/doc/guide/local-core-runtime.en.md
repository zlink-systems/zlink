---
title: "Building Bindings Against a Local Core"
---

# Building Bindings Against a Local Core

Bindings use a verified release package by default. Use a working Core only
explicitly, after building `libzlink` under `core/build`:

```bash
export ZLINK_CORE_SOURCE=local
source bindings/tools/local_core_runtime.sh
```

The helper exports `ZLINK_CORE_INCLUDE_DIR=core/include` and
`ZLINK_CORE_LIB_DIR=core/build/lib`. Build from that same shell. Without
`ZLINK_CORE_SOURCE`, `release` remains the default and the helper fetches and
validates the existing release package path.

## C and C++

Build C++ with `./bindings/cpp/build.sh OFF OFF`. Build C through
`./bindings/c/tests/run_tests.sh` or its CMake entry point. Both CMake projects
use `core/build` and `core/include` for a local Core.

## Rust

Use `./bindings/rust/build.sh`. It passes the local runtime to Cargo; a direct
`cargo build` must retain the helper's sourced environment.

## Node and JavaScript

Rebuild the native Node addon with `cd bindings/node && npm run rebuild-native`.
When local, `binding.gyp` uses the exported include and library directories.
JavaScript samples have no separate native addon and use that built Node addon.

## Python

Use `./bindings/python/build.sh`, or source the helper and run
`cd bindings/python && python3 setup.py build_ext`. The extension compiles and
links with the local include and library directories. Keep release input when
building a distributable wheel.

## Go

Use `./bindings/go/build.sh ./...`. It sets cgo's include, link, and runtime
paths to the local Core. The test and sample runners set the same environment.

## Java and Kotlin

Run `cd bindings/java && ./gradlew build`. In local mode Gradle builds the JNI
bridge with `ZLINK_CORE_INCLUDE_DIR` and `ZLINK_CORE_LIB_DIR`; Kotlin shares
this Java runtime.

## .NET

After sourcing the helper, run `dotnet build bindings/dotnet/Zlink.sln`. Test
and sample runners pass the local runtime to the process loader. In local mode,
`dotnet pack` explicitly uses the Linux native payload from `ZLINK_CORE_LIB_DIR`.

