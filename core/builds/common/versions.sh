#!/bin/bash

# libzlink version definition from the repository source of truth
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIBZLINK_VERSION="$(sed -n 's/^LIBZLINK_VERSION=//p' "$REPO_ROOT/VERSION")"

# Download URLs
LIBZLINK_URL="https://github.com/zlink/libzlink/releases/download/v${LIBZLINK_VERSION}/zlink-${LIBZLINK_VERSION}.tar.gz"

# Export for use in other scripts
export LIBZLINK_VERSION
export LIBZLINK_URL

# Display versions
echo "==================================="
echo "Build Configuration"
echo "==================================="
echo "libzlink version:    ${LIBZLINK_VERSION}"
echo "==================================="
