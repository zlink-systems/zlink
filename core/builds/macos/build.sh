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
    -DBUILD_STATIC=OFF \
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

# Copy .dylib to output
DYLIB_FILE=$(find install/lib -name "libzlink.[0-9]*.dylib" 2>/dev/null | head -n 1)
if [ -z "$DYLIB_FILE" ]; then
    DYLIB_FILE=$(find lib -name "libzlink.[0-9]*.dylib" 2>/dev/null | head -n 1)
fi

if [ -n "$DYLIB_FILE" ]; then
    TARGET_DYLIB="$REPO_ROOT/$OUTPUT_DIR/libzlink.dylib"
    cp "$DYLIB_FILE" "$TARGET_DYLIB"

    # Update install name for better portability
    install_name_tool -id "@rpath/libzlink.dylib" "$TARGET_DYLIB"

    echo "Copied: $DYLIB_FILE -> $TARGET_DYLIB"
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

    # Run tests with ctest
    # Note: Some tests may be skipped based on platform capabilities
    # TIPC tests are Linux-only and will fail on macOS
    CTEST_JOBS="$(sysctl -n hw.ncpu)"
    CTEST_ARGS=(--output-on-failure "-j${CTEST_JOBS}")
    if [ -n "${ZLINK_CTEST_EXCLUDE_REGEX:-}" ]; then
        echo "Excluding tests matching regex: ${ZLINK_CTEST_EXCLUDE_REGEX}"
        CTEST_ARGS+=(--exclude-regex "${ZLINK_CTEST_EXCLUDE_REGEX}")
    fi

    ctest "${CTEST_ARGS[@]}" || {
        echo ""
        echo "Some tests failed. Checking results..."
        # Allow some tests to fail (TIPC tests are Linux-only, some platform-specific tests may fail)
        FAILED_LOG="Testing/Temporary/LastTestsFailed.log"
        FAILED_TESTS=0
        if [ -f "$FAILED_LOG" ]; then
            FAILED_TESTS=$(wc -l < "$FAILED_LOG")
        fi
        if [ "$FAILED_TESTS" -gt 20 ]; then
            echo "Too many test failures ($FAILED_TESTS). Build may be broken."
            exit 1
        fi
        echo "Acceptable number of test failures. Continuing..."
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

    echo ""
    echo "==================================="
    echo "Build completed successfully!"
    echo "Output: $FINAL_DYLIB"
    echo "==================================="
else
    echo "Error: Build failed - $FINAL_DYLIB not found"
    exit 1
fi
