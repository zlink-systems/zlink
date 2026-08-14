#!/bin/sh
#
# VERSION at the repository root is the only release-version source. Public
# headers and package-manager manifests are synchronized by local-package.
#
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION_FILE="$SCRIPT_DIR/../VERSION"
MAJOR="$(sed -n 's/^LIBZLINK_VERSION_MAJOR=//p' "$VERSION_FILE")"
MINOR="$(sed -n 's/^LIBZLINK_VERSION_MINOR=//p' "$VERSION_FILE")"
PATCH="$(sed -n 's/^LIBZLINK_VERSION_PATCH=//p' "$VERSION_FILE")"
VERSION="$(sed -n 's/^LIBZLINK_VERSION=//p' "$VERSION_FILE")"
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || [ "$VERSION" != "$MAJOR.$MINOR.$PATCH" ]; then
    echo "version.sh: error: invalid LIBZLINK_VERSION in $VERSION_FILE" 1>&2
    exit 1
fi
printf '%s' "$VERSION"
