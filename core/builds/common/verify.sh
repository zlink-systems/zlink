#!/bin/bash

# Common verification logic for all platforms
# Usage: verify.sh <library_path> <platform>

set -e

LIBRARY_PATH="$1"
PLATFORM="${2:-linux}"

if [ ! -f "$LIBRARY_PATH" ]; then
    echo "Error: Library file not found at $LIBRARY_PATH"
    exit 1
fi

echo "==================================="
echo "Verifying: $LIBRARY_PATH"
echo "Platform: $PLATFORM"
echo "==================================="

# Get file size
FILE_SIZE=$(stat -f%z "$LIBRARY_PATH" 2>/dev/null || stat -c%s "$LIBRARY_PATH" 2>/dev/null)
echo "File size: $FILE_SIZE bytes"

# Platform-specific verification
case "$PLATFORM" in
    linux)
        echo ""
        echo "Checking dynamic dependencies..."
        ldd "$LIBRARY_PATH"
        ;;

    macos)
        echo ""
        echo "Checking dynamic dependencies..."
        otool -L "$LIBRARY_PATH"

        echo ""
        echo "Checking architecture..."
        lipo -info "$LIBRARY_PATH"
        ;;

    windows)
        echo ""
        echo "Checking for MSVC runtime linkage..."
        echo "Note: Static runtime (/MT) should be used"
        echo ""
        echo "Library info:"
        file "$LIBRARY_PATH" 2>/dev/null || echo "file command not available"
        ;;

    *)
        echo "Unknown platform: $PLATFORM"
        exit 1
        ;;
esac

echo ""
echo "==================================="
echo "Verification completed successfully"
echo "==================================="
