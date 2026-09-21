#!/usr/bin/env bash
# Formats (or checks) the sources the guides excerpt: framework/languages/<lang>/{quickstart,tutorial,samples}.
# One formatter per language, one pinned version each; the rule the output has to satisfy is in
# doc/principal/dev/source-formatting.ko.md. Build scripts (csproj, kts, CMake) are not formatted.
#
#   scripts/format/format.sh [--check] [cpp|dotnet|java|node ...]
#
# With no language every language runs. --check exits 1 when a file would change and rewrites
# nothing. Tool locations:
#   dotnet  CSharpier, pinned in framework/languages/dotnet/.config/dotnet-tools.json
#   node    Prettier,  pinned in framework/languages/node/package.json (devDependencies)
#   java    google-java-format (--aosp) and ktfmt (--kotlinlang-style), pinned below and fetched
#           from Maven Central into .artifacts/format/ on first use
#   cpp     clang-format, major version pinned below; runs from PATH (WSL/Linux: clang-format-18)
set -euo pipefail

GJF_VERSION=1.27.0
KTFMT_VERSION=0.54
CLANG_FORMAT_MAJOR=18

Z=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
CACHE="$Z/.artifacts/format"
CHECK=0
LANGS=()
for arg in "$@"; do
    case "$arg" in
        --check) CHECK=1 ;;
        cpp|dotnet|java|node) LANGS+=("$arg") ;;
        -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done
((${#LANGS[@]})) || LANGS=(cpp dotnet java node)

die() { echo "format: $*" >&2; exit 1; }

#  Every language's files live under these two directories only.
#  Build output never contains sources worth formatting and can be large.
sources() { # <lang> <suffix>...
    local lang=$1; shift
    local root="$Z/framework/languages/$lang"
    local names=() sep=()
    for s in "$@"; do names+=("${sep[@]}" -name "$s"); sep=(-o); done
    find "$root/quickstart" "$root/tutorial" "$root/samples" \
        \( -name node_modules -o -name dist -o -name build -o -name bin -o -name obj -o -name .gradle \) -prune \
        -o -type f \( "${names[@]}" \) -print
}

fetch_jar() { # <name> <url> -> path
    local name=$1 url=$2 path="$CACHE/$1"
    if [ ! -s "$path" ]; then
        mkdir -p "$CACHE"
        curl -fsSL -o "$path.tmp" "$url" || die "cannot download $name"
        mv "$path.tmp" "$path"
    fi
    printf '%s\n' "$path"
}

format_dotnet() {
    local mode=format; ((CHECK)) && mode=check
    (cd "$Z/framework/languages/dotnet" && dotnet tool restore >/dev/null && dotnet csharpier "$mode" quickstart tutorial samples)
}

format_node() {
    local dir="$Z/framework/languages/node" prettier
    if [ -x "$dir/node_modules/.bin/prettier" ]; then
        prettier=("$dir/node_modules/.bin/prettier")
    else
        local version
        version=$(node -p "require('$dir/package.json').devDependencies.prettier")
        prettier=(npx --yes "prettier@$version")
    fi
    local mode=--write; ((CHECK)) && mode=--check
    (cd "$dir" && "${prettier[@]}" "$mode" 'quickstart/**/*.ts' 'tutorial/**/*.ts' 'samples/**/*.ts')
}

format_java() {
    local gjf ktfmt
    gjf=$(fetch_jar "google-java-format-$GJF_VERSION.jar" \
        "https://repo1.maven.org/maven2/com/google/googlejavaformat/google-java-format/$GJF_VERSION/google-java-format-$GJF_VERSION-all-deps.jar")
    ktfmt=$(fetch_jar "ktfmt-$KTFMT_VERSION.jar" \
        "https://repo1.maven.org/maven2/com/facebook/ktfmt/$KTFMT_VERSION/ktfmt-$KTFMT_VERSION-jar-with-dependencies.jar")
    local gjf_mode=(--replace) ktfmt_mode=()
    if ((CHECK)); then gjf_mode=(--dry-run --set-exit-if-changed); ktfmt_mode=(--dry-run --set-exit-if-changed); fi
    #  Called from an `||` list, so `set -e` is off in here: both runs report explicitly.
    local rc=0
    sources java '*.java' | xargs java -jar "$gjf" --aosp "${gjf_mode[@]}" || rc=1
    sources java '*.kt' | xargs java -jar "$ktfmt" --kotlinlang-style "${ktfmt_mode[@]}" || rc=1
    return $rc
}

format_cpp() {
    local bin
    for bin in "clang-format-$CLANG_FORMAT_MAJOR" clang-format; do command -v "$bin" >/dev/null && break; bin=; done
    [ -n "$bin" ] || die "clang-format $CLANG_FORMAT_MAJOR not found (WSL/Linux: apt install clang-format-$CLANG_FORMAT_MAJOR)"
    "$bin" --version | grep -q "version $CLANG_FORMAT_MAJOR\." \
        || die "clang-format major $CLANG_FORMAT_MAJOR required, found: $("$bin" --version)"
    local mode=(-i); ((CHECK)) && mode=(--dry-run -Werror)
    sources cpp '*.cpp' '*.hpp' '*.h' | xargs "$bin" "${mode[@]}"
}

status=0
for lang in "${LANGS[@]}"; do
    echo "== $lang"
    "format_$lang" || { status=1; echo "== $lang: FAILED"; }
done
exit $status
