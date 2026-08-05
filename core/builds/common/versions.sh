#!/bin/bash

# libzlink version definitions
LIBZLINK_VERSION="5.3.3"

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
