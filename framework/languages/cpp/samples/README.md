# ZLink C++ Framework Samples

## Standalone download limitation

A separately downloaded sample cannot find the `zlink` and `zlink-framework`
ports in the default vcpkg registry. Adding this repository's overlay ports
still stops during installation because the published `zlink-cpp` 0.17.6 asset
contains headers but no CMake project or sources. Standalone package-mode
installation therefore cannot complete today. Revalidate installation, build,
and execution after a complete `zlink-cpp` 1.0.0 asset is published. Until then,
use source mode in the repository workspace.
