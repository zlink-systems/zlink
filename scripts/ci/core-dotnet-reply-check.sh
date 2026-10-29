#!/usr/bin/env bash
# Manual macOS candidate verification; never publishes packages or retries failures.
set -euo pipefail

: "${REPLY_CHECK_RESULTS:?}"
: "${RUNNER_TEMP:?}"
: "${GITHUB_WORKSPACE:?}"
mkdir -p "$REPLY_CHECK_RESULTS"
exec > >(tee "$REPLY_CHECK_RESULTS/commands.log") 2>&1
set -x

test "$(uname -m)" = arm64
repo_root=$(pwd -P)
build_root="$repo_root/core/build/reply-check"
package_root="$RUNNER_TEMP/reply-check/packages"
nuget_source="$package_root/nuget"
project=framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj
class=Zlink.Framework.UnitTests.ClientServerLoopOwnershipTests
focused="$class.Stopping_server_delivers_the_reply_of_its_accepted_request"
mkdir -p "$nuget_source"

# Both source identities are immutable for this run, including the harness itself.
{
    printf 'candidate_sha=%s\n' "$(git rev-parse HEAD)"
    printf 'verification_sha=%s\n' "$(git -C "$GITHUB_WORKSPACE/verification" rev-parse HEAD)"
    printf 'focused_runs=20\nclass_runs=1\nfull_unit_suite=%s\n' "$FULL_UNIT_SUITE"
    git status --porcelain
    cat VERSION bindings/dotnet/VERSION framework/languages/dotnet/VERSION
    uname -a
    xcodebuild -version
    clang++ --version
    dotnet --info
} > "$REPLY_CHECK_RESULTS/source-and-tools.txt"

/opt/homebrew/bin/brew install cmake pkg-config openssl@3
export OPENSSL_ROOT_DIR
OPENSSL_ROOT_DIR=$(/opt/homebrew/bin/brew --prefix openssl@3)
export CMAKE_PREFIX_PATH="$OPENSSL_ROOT_DIR"
export LDFLAGS="-L$OPENSSL_ROOT_DIR/lib"
export CPPFLAGS="-I$OPENSSL_ROOT_DIR/include"
export PKG_CONFIG_PATH="$OPENSSL_ROOT_DIR/lib/pkgconfig"

# Same native architecture/OpenSSL/C++ settings as core/builds/macos/build.sh.
# Build only the shared library with release LTO; this is not a Core gate.
cmake -S core -B "$build_root" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    "-DOPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR" \
    "-DOPENSSL_LIBRARIES=$OPENSSL_ROOT_DIR/lib/libssl.dylib;$OPENSSL_ROOT_DIR/lib/libcrypto.dylib" \
    "-DOPENSSL_INCLUDE_DIR=$OPENSSL_ROOT_DIR/include" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED=ON -DBUILD_STATIC=OFF -DBUILD_TESTS=OFF -DENABLE_LTO=ON \
    -DZLINK_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_MACOSX_RPATH=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build_root" --target libzlink --parallel "$(sysctl -n hw.ncpu)" --verbose
export ZLINK_LIBRARY_PATH
ZLINK_LIBRARY_PATH=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$build_root/lib/libzlink.dylib")
test -s "$ZLINK_LIBRARY_PATH"
test "$(lipo -archs "$ZLINK_LIBRARY_PATH")" = arm64
{
    printf 'ZLINK_LIBRARY_PATH=%s\n' "$ZLINK_LIBRARY_PATH"
    file "$ZLINK_LIBRARY_PATH"
    otool -L "$ZLINK_LIBRARY_PATH"
    shasum -a 256 "$ZLINK_LIBRARY_PATH"
    cmake --version
} > "$REPLY_CHECK_RESULTS/native-provenance.txt"

# The existing public binding pack contract requires a Linux/Windows runtime.
# Its verified Linux payload is package input only, never the macOS test runtime.
core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/dotnet/VERSION)
test -n "$core_version"
test -n "$binding_version"
core_prefix=$(bash scripts/local-package/core/fetch-release.sh \
    --version "$core_version" --platform linux-x64 --cache-dir .artifacts/core-release)
dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj -c Release -o "$nuget_source" \
    "-p:ZLinkCoreVersion=$core_version" -p:ZLinkCoreSource=local \
    "-p:ZLinkLinuxX64NativeRoot=$core_prefix/lib" \
    "-p:ZLinkCoreProvenancePath=$core_prefix/share/zlink/core-package-provenance.json"
test -s "$nuget_source/Zlink.$binding_version.nupkg"
cp "$core_prefix/share/zlink/core-package-provenance.json" \
    "$REPLY_CHECK_RESULTS/binding-linux-payload-provenance.json"
dotnet pack framework/languages/dotnet/src/Zlink.HttpClient/Zlink.HttpClient.csproj \
    -c Release -o "$nuget_source"
export ZLINK_LOCAL_PACKAGE_ROOT="$package_root"
export ZLinkLocalPackageRoot="$package_root"
shasum -a 256 "$nuget_source"/*.nupkg > "$REPLY_CHECK_RESULTS/package-sha256.txt"

dotnet restore "$project"
dotnet build "$project" -c Release -f net10.0 --no-restore
managed_output=framework/languages/dotnet/tests/Zlink.Framework.UnitTests/bin/Release/net10.0
shasum -a 256 "$managed_output/Systems.Zlink.dll" "$managed_output/Zlink.Framework.dll" \
    "$managed_output/Zlink.Framework.UnitTests.dll" > "$REPLY_CHECK_RESULTS/managed-sha256.txt"
# No packaged macOS fallback may mask failure to load our configured source dylib.
if find framework/languages/dotnet -path '*/runtimes/osx-*/native/*' -name '*zlink*.dylib' -print | grep .; then
    echo 'Unexpected packaged macOS native fallback' >&2
    exit 1
fi

run_tests() {
    local name="$1" filter="$2" expected="$3"
    local results="$REPLY_CHECK_RESULTS/$name"
    mkdir -p "$results"
    local args=("$project" -c Release -f net10.0 --no-build --no-restore
        --logger 'console;verbosity=detailed' --logger 'trx;LogFileName=test.trx'
        --results-directory "$results" --diag "$results/vstest.log"
        --blame-hang --blame-hang-timeout 10m --blame-hang-dump-type full)
    if [ -n "$filter" ]; then args+=(--filter "$filter"); fi
    # dyld records the actual image loaded by the testhost, without a binding API change.
    # Preserve the first nonzero test exit through pipefail; later runs never start.
    DYLD_PRINT_LIBRARIES=1 dotnet test "${args[@]}" 2>&1 | tee "$results/console.log"
    python3 - "$results" "$ZLINK_LIBRARY_PATH" <<'PY'
import pathlib
import re
import sys
results = pathlib.Path(sys.argv[1])
pattern = re.compile(r"dyld\[\d+\]:[^\r\n]*" + re.escape(sys.argv[2]))
evidence = []
for path in results.rglob("*"):
    if path.is_file() and path.suffix in {".log", ".trx", ".txt"}:
        evidence.extend(pattern.findall(path.read_text(errors="replace")))
assert evidence, "No dyld image-load evidence for the candidate native library"
(results / "loaded-native-images.txt").write_text("\n".join(evidence) + "\n")
PY
    python3 - "$results/test.trx" "$expected" "$focused" <<'PY'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
ns = {'t': 'http://microsoft.com/schemas/VisualStudio/TeamTest/2010'}
results = root.findall('.//t:UnitTestResult', ns)
assert results, 'No test results: filter may not match'
assert all(r.get('outcome') == 'Passed' for r in results), 'Failed/skipped test result'
if sys.argv[2] != 'any':
    assert len(results) == int(sys.argv[2]), f'Expected {sys.argv[2]} tests, got {len(results)}'
assert any(r.get('testName') == sys.argv[3] for r in results), 'Original accepted-request test did not run'
print(f'Validated {len(results)} passed tests, including original accepted-request test')
PY
}

# Predeclared repetitions are independent runs; a failure stops the sequence.
for iteration in $(seq 1 20); do
    run_tests "focused-$iteration" "FullyQualifiedName=$focused" 1
done
run_tests class "FullyQualifiedName~$class" 3
if [ "$FULL_UNIT_SUITE" = true ]; then
    # Use the test framework's existing worker configuration, as in the original run.
    run_tests full-unit-suite '' any
fi
shasum -a 256 "$ZLINK_LIBRARY_PATH" > "$REPLY_CHECK_RESULTS/native-sha256-after-tests.txt"
