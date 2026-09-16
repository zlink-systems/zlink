#!/usr/bin/env bash
# Fail when a distributed static archive exports anything but the public C ABI.
#
# Usage: check_static_archive_exports.sh <archive> <version-script>
# Environment: NM (defaults to nm)
#
# The shared library hides its vendored Boost behind a version script. An
# archive has no such filter, so every Boost template instantiation in it is an
# ordinary external definition that the consumer's linker may pick over the
# consumer's own Boost -- or the other way round. Either way the two sides then
# disagree about layout: boost::asio::io_context is 16 bytes in the Boost Core
# vendors and 24 in a newer one, and the first zlink_bind() segfaults inside
# service_registry::do_use_service (issue #418). This check is what keeps the
# archive's external surface equal to the shared library's.
set -euo pipefail

archive=${1:?usage: check_static_archive_exports.sh <archive> <version-script>}
version_script=${2:?usage: check_static_archive_exports.sh <archive> <version-script>}
nm_tool=${NM:-nm}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

[ -r "$archive" ] || {
    echo "static archive not readable: $archive" >&2
    exit 2
}

prefix=""
case "$(uname -s)" in
Darwin) prefix="_" ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$here/static_archive_public_symbols.sh" "$version_script" "$prefix" > "$work/allowed"

# --format=posix prints "<name> <type> <value> <size>" per defined external
# symbol, plus one "<archive>[<member>]:" header line per member. The header
# has a single field, so the type-letter test drops it.
"$nm_tool" --defined-only --extern-only --format=posix "$archive" \
    | awk 'NF >= 2 && length($2) == 1 { print $1 }' \
    | sort -u > "$work/exported"

comm -13 "$work/allowed" "$work/exported" > "$work/unexpected"
missing=$(comm -23 "$work/allowed" "$work/exported" | wc -l | tr -d ' ')
unexpected=$(wc -l < "$work/unexpected" | tr -d ' ')
exported=$(wc -l < "$work/exported" | tr -d ' ')

if [ "$unexpected" -ne 0 ]; then
    echo "$archive exports $unexpected symbol(s) outside the public C ABI." >&2
    echo "Internal C++ and vendored Boost definitions must not be visible to a" >&2
    echo "consumer's link; see issue #418. First 20:" >&2
    head -20 "$work/unexpected" >&2
    boost=$(grep -c 'boost' "$work/unexpected" || true)
    echo "(of which $boost name boost)" >&2
    exit 1
fi

if [ "$missing" -ne 0 ]; then
    echo "$archive is missing $missing public C ABI symbol(s):" >&2
    comm -23 "$work/allowed" "$work/exported" | head -20 >&2
    exit 1
fi

echo "OK: $(basename "$archive") exports $exported symbols, all of them the public C ABI."
