#!/bin/bash

# macOS build script for libzlink
# Supports both x86_64 and arm64 architectures
# Requires: Xcode Command Line Tools, cmake
#
set -e

# Get script directory and repo root early (before any cd commands)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# Read the only release-version source.
[ -f "$REPO_ROOT/VERSION" ] || {
    echo "Repository VERSION file not found: $REPO_ROOT/VERSION" >&2
    exit 2
}
LIBZLINK_VERSION=$(grep '^LIBZLINK_VERSION=' "$REPO_ROOT/VERSION" | cut -d'=' -f2)

# Parse arguments: ARCH RUN_TESTS
ARCH="${1:-$(uname -m)}"
RUN_TESTS="${2:-OFF}"
BUILD_TYPE="Release"
OUTPUT_DIR="core/dist/macos-${ARCH}"

# Normalize architecture name
if [ "$ARCH" = "x64" ]; then
    ARCH="x86_64"
fi

# Validate architecture
if [ "$ARCH" != "x86_64" ] && [ "$ARCH" != "arm64" ]; then
    echo "Error: Invalid architecture '$ARCH'. Use 'x86_64' or 'arm64'"
    exit 1
fi

echo ""
echo "==================================="
echo "macOS Build Configuration"
echo "==================================="
echo "Architecture:      ${ARCH}"
echo "libzlink version:    ${LIBZLINK_VERSION}"
echo "RUN_TESTS:         ${RUN_TESTS}"
echo "Build type:        ${BUILD_TYPE}"
echo "Output directory:  ${OUTPUT_DIR}"
echo "==================================="
echo ""

# Change to repo root
cd "$REPO_ROOT"

# Create build directories
BUILD_DIR="core/build/macos-${ARCH}"
mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

echo "Step 1: Using local repository source for libzlink..."

# Step 2: Configure libzlink with CMake for ${ARCH}
echo ""
echo "Step 2: Configuring libzlink with CMake for ${ARCH}..."
cd "$BUILD_DIR"

LIBZLINK_SRC_ABS="$REPO_ROOT/core"

# Set architecture-specific CMake flags
CMAKE_ARCH_FLAGS="-DCMAKE_OSX_ARCHITECTURES=$ARCH"

# Determine BUILD_TESTS flag
BUILD_TESTS_FLAG="OFF"
if [ "$RUN_TESTS" = "ON" ]; then
    BUILD_TESTS_FLAG="ON"
fi
# The static archive (libzlink.a) is always built: internal contract tests
# use private Core helpers not exported from the distributable dylib, and
# the release archive ships it so vcpkg's default static triplet can
# consume the install prefix instead of building Core from source
# (issue #397).
BUILD_STATIC_FLAG="ON"

# Configure build
if [ -z "$OPENSSL_ROOT_DIR" ]; then
    if command -v brew >/dev/null 2>&1; then
        OPENSSL_ROOT_DIR=$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)
    fi
fi

CMAKE_OPENSSL_ARGS=""
if [ -n "$OPENSSL_ROOT_DIR" ]; then
    echo "Using OpenSSL from: $OPENSSL_ROOT_DIR"
    CMAKE_OPENSSL_ARGS="-DOPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR -DOPENSSL_LIBRARIES=$OPENSSL_ROOT_DIR/lib/libssl.dylib;$OPENSSL_ROOT_DIR/lib/libcrypto.dylib -DOPENSSL_INCLUDE_DIR=$OPENSSL_ROOT_DIR/include"
fi

cmake "$LIBZLINK_SRC_ABS" \
    $CMAKE_ARCH_FLAGS \
    $CMAKE_OPENSSL_ARGS \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED=ON \
    -DBUILD_STATIC="$BUILD_STATIC_FLAG" \
    -DBUILD_TESTS="$BUILD_TESTS_FLAG" \
    -DZLINK_CXX_STANDARD=17 \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/install" \
    -DCMAKE_MACOSX_RPATH=ON

# Step 3: Build libzlink
echo ""
echo "Step 3: Building libzlink for ${ARCH}..."
make -j$(sysctl -n hw.ncpu)

# Step 4: Install
echo ""
echo "Step 4: Installing to output directory..."
make install

# Merge the full `make install` prefix (lib/cmake/zlink/*.cmake, libzlink.a,
# lib/pkgconfig/libzlink.pc, the complete include/ tree) into the output
# directory. This is additive: it sits alongside the flat libzlink.dylib /
# include/*.h files the steps below produce, which existing consumers
# (fetch-release.sh, node/java prebuild downloads, framework CI) still read
# from the archive root.
echo ""
echo "Step 4b: Merging install prefix into output directory..."
cp -a install/. "$REPO_ROOT/$OUTPUT_DIR/"

# The macOS release owns its complete runtime closure. Keep the versioned Core
# dylib and its relative symlinks together, bundle Homebrew OpenSSL, rewrite
# every non-system load command to the sibling in lib/, then re-sign every
# modified Mach-O file.
PACKAGE_LIB="$REPO_ROOT/$OUTPUT_DIR/lib"
DYLIB_FILE=$(find "$PACKAGE_LIB" -type f -name "libzlink.[0-9]*.dylib" | head -n 1)
OPENSSL_SSL="$OPENSSL_ROOT_DIR/lib/libssl.3.dylib"
OPENSSL_CRYPTO="$OPENSSL_ROOT_DIR/lib/libcrypto.3.dylib"

if [ ! -f "$OPENSSL_SSL" ] || [ ! -f "$OPENSSL_CRYPTO" ]; then
    echo "Error: OpenSSL runtime closure is missing under $OPENSSL_ROOT_DIR/lib" >&2
    exit 1
fi
cp -a "$OPENSSL_SSL" "$OPENSSL_CRYPTO" "$PACKAGE_LIB/"

if [ -n "$DYLIB_FILE" ]; then
    TARGET_DYLIB="$REPO_ROOT/$OUTPUT_DIR/libzlink.dylib"
    # The Core dylib keeps an @rpath install name: a consumer links it through its
    # own rpath (lib/), while the OpenSSL siblings inside lib/ resolve via @loader_path.
    install_name_tool -id "@rpath/libzlink.0.dylib" "$DYLIB_FILE"

    for binary in "$DYLIB_FILE" "$PACKAGE_LIB/libssl.3.dylib" "$PACKAGE_LIB/libcrypto.3.dylib"; do
        while IFS= read -r dependency; do
            case "$(basename "$dependency")" in
                libssl.3.dylib|libcrypto.3.dylib)
                    install_name_tool -change "$dependency" \
                        "@loader_path/$(basename "$dependency")" "$binary"
                    ;;
            esac
        done < <(otool -L "$binary" | tail -n +2 | sed 's/^[[:space:]]*//' | cut -d' ' -f1)
    done
    install_name_tool -id "@loader_path/libssl.3.dylib" "$PACKAGE_LIB/libssl.3.dylib"
    install_name_tool -id "@loader_path/libcrypto.3.dylib" "$PACKAGE_LIB/libcrypto.3.dylib"
    codesign --force --sign - "$PACKAGE_LIB/libcrypto.3.dylib"
    codesign --force --sign - "$PACKAGE_LIB/libssl.3.dylib"
    codesign --force --sign - "$DYLIB_FILE"

    ln -sfn "lib/libzlink.0.dylib" "$TARGET_DYLIB"
    echo "Packaged relocatable runtime closure in: $PACKAGE_LIB"
else
    echo "Error: libzlink.dylib not found!"
    exit 1
fi

# Copy public headers
echo ""
echo "Copying public headers..."
INCLUDE_DIR="$REPO_ROOT/$OUTPUT_DIR/include"
mkdir -p "$INCLUDE_DIR"
cp install/include/zlink.h "$INCLUDE_DIR/"
cp install/include/zlink_enum.h "$INCLUDE_DIR/"
cp install/include/zlink_errno.h "$INCLUDE_DIR/"
echo "Copied public headers -> $INCLUDE_DIR/"

cd "$REPO_ROOT"

# Step 5: Run tests (if enabled)
if [ "$RUN_TESTS" = "ON" ]; then
    echo ""
    echo "Step 5: Running tests..."
    cd "$BUILD_DIR"

    # Build test executables
    make -j$(sysctl -n hw.ncpu)

    # Run tests with ctest. Tests labelled "serial" are run one at a time
    # (they share ports and timing budgets); the rest run in parallel. Any
    # failure fails the build, like the Linux and Windows CI jobs.
    CTEST_JOBS="$(sysctl -n hw.ncpu)"
    CTEST_COMMON=(--output-on-failure)
    if [ -n "${ZLINK_CTEST_EXCLUDE_REGEX:-}" ]; then
        echo "Excluding tests matching regex: ${ZLINK_CTEST_EXCLUDE_REGEX}"
        CTEST_COMMON+=(--exclude-regex "${ZLINK_CTEST_EXCLUDE_REGEX}")
    fi
    ctest "${CTEST_COMMON[@]}" -L serial -j1 || {
        echo "Serial tests failed (see Testing/Temporary/LastTestsFailed.log)."
        exit 1
    }
    ctest "${CTEST_COMMON[@]}" -LE serial "-j${CTEST_JOBS}" || {
        echo "Parallel tests failed (see Testing/Temporary/LastTestsFailed.log)."
        exit 1
    }

    cd "$REPO_ROOT"
fi

# Step 6: Verify build
echo ""
echo "Step 6: Verifying build for ${ARCH}..."
FINAL_DYLIB="$OUTPUT_DIR/libzlink.dylib"

if [ -f "$FINAL_DYLIB" ]; then
    echo "File size: $(stat -f%z "$FINAL_DYLIB") bytes"

    # Verify architecture
    echo "Architecture verification:"
    lipo -info "$FINAL_DYLIB"

    bash scripts/local-package/core/verify-package.sh \
        --prefix "$REPO_ROOT/$OUTPUT_DIR"

    echo ""
    echo "==================================="
    echo "Build completed successfully!"
    echo "Output: $FINAL_DYLIB"
    echo "==================================="
else
    echo "Error: Build failed - $FINAL_DYLIB not found"
    exit 1
fi
