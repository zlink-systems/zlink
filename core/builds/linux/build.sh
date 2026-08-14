#!/bin/bash

# Linux build script for libzlink
# Supports both x64 and arm64 architectures
# Requires: gcc, g++, make, cmake, pkg-config
#
set -e

# Get script directory and repo root early (before any cd commands)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NORMALIZE_TIMESTAMPS_SH="$REPO_ROOT/core/tools/normalize_build_timestamps.sh"
export TMPDIR=/tmp
MAKE_BIN="$(command -v gmake || command -v make)"

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

# Normalize architecture name
# Convert from uname -m format to our naming convention
if [ "$ARCH" = "x86_64" ]; then
    ARCH="x64"
elif [ "$ARCH" = "aarch64" ]; then
    ARCH="arm64"
fi

# Validate architecture
if [ "$ARCH" != "x64" ] && [ "$ARCH" != "arm64" ]; then
    echo "Error: Invalid architecture '$ARCH'. Use 'x64' or 'arm64'"
    exit 1
fi

OUTPUT_DIR="core/dist/linux-${ARCH}"

echo ""
echo "==================================="
echo "Linux Build Configuration"
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
BUILD_DIR="core/build/linux-${ARCH}"
mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

echo "Step 1: Using repository source for libzlink..."

# Step 2: Configure libzlink with CMake
echo ""
echo "Step 2: Configuring libzlink with CMake for ${ARCH}..."
cd "$BUILD_DIR"

LIBZLINK_SRC_ABS="$REPO_ROOT/core"

# Set architecture-specific CMake flags for cross-compilation if needed
CMAKE_ARCH_FLAGS=""
if [ "$ARCH" = "arm64" ]; then
    CMAKE_ARCH_FLAGS="-DCMAKE_SYSTEM_PROCESSOR=aarch64"
elif [ "$ARCH" = "x64" ]; then
    CMAKE_ARCH_FLAGS="-DCMAKE_SYSTEM_PROCESSOR=x86_64"
fi

# Determine BUILD_TESTS flag
BUILD_TESTS_FLAG="OFF"
if [ "$RUN_TESTS" = "ON" ]; then
    BUILD_TESTS_FLAG="ON"
fi

# Unit tests in core/tests/unittest are wired when BUILD_STATIC is enabled.
# Keep default packaging build shared-only, but include static lib for test runs.
BUILD_STATIC_FLAG="OFF"
if [ "$RUN_TESTS" = "ON" ]; then
    BUILD_STATIC_FLAG="ON"
fi

# Configure build
cmake "$LIBZLINK_SRC_ABS" \
    $CMAKE_ARCH_FLAGS \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_BIN" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_SHARED=ON \
    -DBUILD_STATIC="$BUILD_STATIC_FLAG" \
    -DBUILD_TESTS="$BUILD_TESTS_FLAG" \
    -DBUILD_BENCHMARKS=OFF \
    -DZLINK_CXX_STANDARD=17 \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/install"

if [ -f "compile_commands.json" ]; then
    ln -sfn "$BUILD_DIR/compile_commands.json" "$REPO_ROOT/compile_commands.json"
    echo "Linked compile_commands.json -> $BUILD_DIR/compile_commands.json"
fi

# Step 3: Build libzlink
echo ""
echo "Step 3: Building libzlink for ${ARCH}..."
bash "$NORMALIZE_TIMESTAMPS_SH" "$BUILD_DIR"
make -j$(nproc)

# Step 4: Install
echo ""
echo "Step 4: Installing to output directory..."
make install

# Copy .so to output (version-agnostic: matches any major, e.g. libzlink.so.6.0.1)
SO_FILE=$(find install/lib* -name "libzlink.so.[0-9]*" 2>/dev/null | head -n 1)
if [ -z "$SO_FILE" ]; then
    SO_FILE=$(find lib -name "libzlink.so.[0-9]*" 2>/dev/null | head -n 1)
fi

if [ -n "$SO_FILE" ]; then
    TARGET_SO="$REPO_ROOT/$OUTPUT_DIR/libzlink.so"
    cp "$SO_FILE" "$TARGET_SO"
    echo "Copied: $SO_FILE -> $TARGET_SO"
    # Derive SOVERSION (major) from the built file name, e.g. libzlink.so.6.0.1 -> 6
    SO_VERSION="$(basename "$SO_FILE")"
    SO_VERSION="${SO_VERSION#libzlink.so.}"
    SO_MAJOR="${SO_VERSION%%.*}"
    ln -sfn "libzlink.so" "$REPO_ROOT/$OUTPUT_DIR/libzlink.so.${SO_MAJOR}"
    echo "Linked: $REPO_ROOT/$OUTPUT_DIR/libzlink.so.${SO_MAJOR} -> libzlink.so"
else
    echo "Error: libzlink.so not found!"
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
    TEST_DIR="$BUILD_DIR"
    bash "$REPO_ROOT/core/tests/run_test_lanes.sh" \
        --build-dir "$TEST_DIR" \
        --include-e2e
fi

# Step 6: Verify build
echo ""
echo "Step 6: Verifying build for ${ARCH}..."
FINAL_SO="$OUTPUT_DIR/libzlink.so"

if [ -f "$FINAL_SO" ]; then
    echo "File size: $(stat -c%s "$FINAL_SO") bytes"

    # Verify architecture using readelf (if available)
    if command -v readelf &> /dev/null; then
        echo "Architecture verification:"
        MACHINE=$(readelf -h "$FINAL_SO" | grep Machine | awk '{print $2}')
        echo "ELF Machine: $MACHINE"
    fi

    echo ""
    echo "==================================="
    echo "Build completed successfully!"
    echo "Output: $FINAL_SO"
    echo "==================================="
else
    echo "Error: Build failed - $FINAL_SO not found"
    exit 1
fi
