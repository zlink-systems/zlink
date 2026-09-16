#!/usr/bin/env bash
# Rewrite a just-built libzlink.a so only the public zlink_* C ABI stays
# visible to a consumer's link.
#
# Usage: hide_static_archive_internals.sh <archive> <version-script>
# Environment: AR, RANLIB, LD, NM, OBJCOPY (each defaults to the bare tool name)
#
# Why the archive needs this at all: the shared library hides Core's vendored
# Boost behind core/src/libzlink.vers, so a consumer linking libzlink.so can
# never see it. The archive carried the same Boost as ~2,400 ordinary external
# definitions, and a consumer that compiles its own Boost from different
# headers collapses the two onto one definition. boost::asio::io_context is 16
# bytes in Boost 1.85 and 24 in 1.92, so the surviving definition is then used
# with the other side's layout and the first zlink_bind() segfaults inside
# service_registry::do_use_service (issue #418).
#
# Both platforms first partial-link every member into one relocatable object.
# That is not an optimisation: a symbol can only be made local once nothing
# outside its own object still refers to it, and Core's members refer to each
# other constantly.
set -euo pipefail

archive=${1:?usage: hide_static_archive_internals.sh <archive> <version-script>}
version_script=${2:?usage: hide_static_archive_internals.sh <archive> <version-script>}
ar_tool=${AR:-ar}
ranlib_tool=${RANLIB:-ranlib}
ld_tool=${LD:-ld}
nm_tool=${NM:-nm}
objcopy_tool=${OBJCOPY:-objcopy}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

[ -w "$archive" ] || {
    echo "static archive not writable: $archive" >&2
    exit 2
}

archive=$(cd "$(dirname "$archive")" && pwd)/$(basename "$archive")
merged_name=$(basename "$archive")
merged_name=${merged_name%.*}.o
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# A macOS archive carries its symbol index as a member named "__.SYMDEF" (or
# "__.SYMDEF SORTED"). It is not an object file: ar extracts it, ld refuses it
# with "unknown-unsupported file format", and ranlib rebuilds it for the archive
# this script writes. Drop it here rather than feeding it to the merge.
"$ar_tool" t "$archive" | grep -v '^__\.SYMDEF' > "$work/members"
if [ ! -s "$work/members" ]; then
    echo "$archive has no members" >&2
    exit 2
fi
if [ "$(sort "$work/members" | uniq -d | wc -l)" -ne 0 ]; then
    echo "$archive has members with duplicate names; extracting would lose one:" >&2
    sort "$work/members" | uniq -d >&2
    exit 2
fi

mkdir "$work/members.d"
(cd "$work/members.d" && "$ar_tool" x "$archive")

# Not mapfile: macOS still ships bash 3.2, which does not have it.
objects=()
while IFS= read -r member; do
    [ -n "$member" ] || continue
    [ -f "$work/members.d/$member" ] || {
        echo "$ar_tool x did not produce $member" >&2
        exit 2
    }
    objects+=("$work/members.d/$member")
done < "$work/members"

case "$(uname -s)" in
Darwin)
    # ld64 turns private extern symbols into static ones during a relocatable
    # link unless -keep_private_externs is passed, so this merge is the hiding
    # step. Compile-time hidden visibility alone does not decide what is private
    # extern here: template and inline statics get vague (weak/coalesced)
    # linkage and stay external through it -- a first attempt left 104 of them,
    # Boost.Asio service ids and openssl_init guards among them. Name the public
    # surface instead and let everything outside it become non-external, from
    # the same version script the ELF path and the shared library use. Mach-O
    # prefixes C symbols with an underscore, hence the prefix argument.
    "$here/static_archive_public_symbols.sh" "$version_script" _ > "$work/keep"
    "$ld_tool" -r -exported_symbols_list "$work/keep" \
        -o "$work/$merged_name" "${objects[@]}"
    ;;
*)
    # ELF hidden visibility would not do here: it only controls what a shared
    # object exports, and a static link resolves hidden and default symbols in
    # one flat namespace. The symbols have to become STB_LOCAL.
    #
    # --force-group-allocation deletes the COMDAT groups after resolving them.
    # Without it the groups survive into the merged object, the consumer's
    # linker discards Core's copy in favour of its own identically named
    # group, and the link fails on Core's now-local symbols pointing into a
    # discarded section.
    "$ld_tool" -r --force-group-allocation -o "$work/$merged_name" "${objects[@]}"

    "$here/static_archive_public_symbols.sh" "$version_script" > "$work/keep"

    localize() {
        "$objcopy_tool" --keep-global-symbols="$work/keep" \
            "$work/$merged_name" "$work/rewritten.o"
        mv "$work/rewritten.o" "$work/$merged_name"
    }
    survivors() {
        "$nm_tool" --defined-only --extern-only --format=posix \
            "$work/$merged_name" \
            | awk 'NF >= 2 && length($2) == 1 { print $1 }' \
            | sort -u | comm -23 - "$work/keep"
    }

    localize
    # The build compiles these sources with -fno-gnu-unique (see core/CMakeLists.txt),
    # so the STB_GNU_UNIQUE binding objcopy refuses to localize should not appear
    # at all. Keep the weaken-then-localize fallback anyway, for a toolchain that
    # produces some other binding the first pass will not take, and repeat it
    # while it keeps making progress rather than exactly once: one round was
    # enough on binutils 2.42 and left 276 symbols behind on 2.38. Stop when a
    # round removes nothing, so a binding neither pass can move fails the check
    # below instead of looping.
    remaining=$(survivors | wc -l)
    while [ "$remaining" -gt 0 ]; do
        survivors > "$work/weaken"
        "$objcopy_tool" --weaken-symbols="$work/weaken" \
            "$work/$merged_name" "$work/weakened.o"
        mv "$work/weakened.o" "$work/$merged_name"
        localize
        still=$(survivors | wc -l)
        [ "$still" -lt "$remaining" ] || break
        remaining=$still
    done
    ;;
esac

rm -f "$archive"
"$ar_tool" cr "$archive" "$work/$merged_name"
"$ranlib_tool" "$archive"

NM="$nm_tool" "$here/check_static_archive_exports.sh" "$archive" "$version_script"
