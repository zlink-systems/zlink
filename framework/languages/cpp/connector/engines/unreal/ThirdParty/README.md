# Unreal native package

`ZLinkStreamConnector.Build.cs` consumes a generated package under
`ThirdParty/ZLink/`. The package is intentionally not committed because its
libraries depend on the Unreal target platform and toolchain.

Prepare it from a configured and built CMake tree:

```bash
cmake \
  -DZLINK_UNREAL_BUILD_DIR=/absolute/path/to/framework/languages/cpp/build \
  -DZLINK_UNREAL_OUTPUT_DIR=/absolute/path/to/ThirdParty/ZLink \
  -DZLINK_UNREAL_CONFIGURATION=Release \
  -P framework/languages/cpp/connector/engines/unreal/Tools/package-third-party.cmake
```

The build must use `ZLINK_STREAM_CONNECTOR_BUILD_UNREAL=ON`. The script installs
only the `StreamConnector` CMake component and stages the Unreal adapter, C++
binding, Core runtime and Core CMake package, optional OpenSSL/LZ4 libraries, and
a relative `zlink-unreal-package.manifest`. The manifest records the producer platform,
architecture, configuration, compiler, and C++ standard. Unreal Build Tool reads
the manifest and validates the target metadata and every path before adding it to
the module. A project can use another package location by setting
`ZLINK_UNREAL_THIRDPARTY_ROOT` to the directory containing the manifest.

The generated CMake export uses package-relative dependency paths. On Windows,
the package contains both the Core DLL and its import library, so a clean consumer
does not use an absolute path into the producer's build tree.

Before running Unreal Build Tool, set `ZLINK_UNREAL_COMPILER_ID` and
`ZLINK_UNREAL_COMPILER_VERSION` to the actual Unreal target toolchain values. They
must match the manifest; the module requires both values and rejects a package
built with a different compiler or CRT. System libraries are selected from the
staged target platform and the Core package export, rather than from the host
that runs the packaging script.

The generated package is a build artifact. Keep its headers and libraries out
of source control; only this layout and the package script belong in the
plugin source tree.
