#!/usr/bin/env bash
# Print the public symbol names a distributed static archive may export, one per
# line, read from the linker version script the shared library already uses.
# That script is the single source of truth for the public surface: the shared
# library exports exactly these names and nothing else, so the archive holds to
# the same list.
#
# Usage: static_archive_public_symbols.sh <version-script> [symbol-prefix [archive]]
# Environment: NM (defaults to nm), used only when an archive is given
#
# Mach-O prefixes every C symbol with an underscore, so callers on Apple pass
# "_" as the prefix.
#
# A version script names its exports in two forms:
#   - plain C names (`zlink_bind;`), printed as they are;
#   - mangled C++ name globs, written as `# APPLE_EXPORT: <glob>` comment lines
#     in Mach-O spelling (`__ZN5zlink*`) next to the `extern "C++"` block that
#     says the same thing for GNU ld (framework/languages/cpp/cmake/
#     zlink-framework-shared-symbols.map). A glob names no symbol until it is
#     matched against something, so the caller passes the archive and the
#     output lists the external definitions in it that a glob matches.
set -euo pipefail

version_script=${1:?usage: static_archive_public_symbols.sh <version-script> [prefix [archive]]}
prefix=${2:-}
archive=${3:-}
nm_tool=${NM:-nm}

[ -r "$version_script" ] || {
    echo "version script not readable: $version_script" >&2
    exit 2
}

names=$(sed -n '/global:/,/local:/p' "$version_script" \
    | sed -n 's/^[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\);.*/\1/p' \
    | sort -u)

globs=$(sed -n 's/^#[[:space:]]*APPLE_EXPORT:[[:space:]]*//p' "$version_script" \
    | sed "s/^_/${prefix}/")

[ -n "$names" ] || [ -n "$globs" ] || {
    echo "no global entries found in $version_script" >&2
    exit 2
}

{
if [ -n "$names" ]; then
    printf '%s\n' "$names" | sed "s/^/${prefix}/"
fi

if [ -n "$globs" ]; then
    [ -r "$archive" ] || {
        echo "$version_script names symbols by glob; pass the archive to match them against" >&2
        exit 2
    }
    # --format=posix prints "<name> <type> <value> <size>" per defined external
    # symbol, plus one "<archive>[<member>]:" header line per member. The header
    # has a single field, so the type-letter test drops it.
    "$nm_tool" --defined-only --extern-only --format=posix "$archive" \
        | awk 'NF >= 2 && length($2) == 1 { print $1 }' \
        | sort -u \
        | awk -v globs="$globs" '
            BEGIN {
                n = split(globs, glob, "\n")
                for (i = 1; i <= n; i++) {
                    if (glob[i] == "") continue
                    gsub(/\*/, ".*", glob[i])
                    pattern[++count] = "^" glob[i] "$"
                }
            }
            { for (i = 1; i <= count; i++) if ($0 ~ pattern[i]) { print; next } }'
fi
} | sort -u
