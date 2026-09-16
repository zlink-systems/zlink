#!/usr/bin/env bash
# Print the public C ABI names a distributed static archive may export, one per
# line, read from the linker version script the shared library already uses.
# That script is the single source of truth for the public surface: the shared
# library exports exactly these names and nothing else, so the archive holds to
# the same list.
#
# Usage: static_archive_public_symbols.sh <version-script> [symbol-prefix]
#
# Mach-O prefixes every C symbol with an underscore, so callers on Apple pass
# "_" as the prefix.
set -euo pipefail

version_script=${1:?usage: static_archive_public_symbols.sh <version-script> [prefix]}
prefix=${2:-}

[ -r "$version_script" ] || {
    echo "version script not readable: $version_script" >&2
    exit 2
}

names=$(sed -n '/global:/,/local:/p' "$version_script" \
    | sed -n 's/^[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\);.*/\1/p' \
    | sort -u)

[ -n "$names" ] || {
    echo "no global entries found in $version_script" >&2
    exit 2
}

printf '%s\n' "$names" | sed "s/^/${prefix}/"
