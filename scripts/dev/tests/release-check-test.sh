#!/usr/bin/env bash

set -euo pipefail

TEST_ROOT=/tmp/zlink-release-check-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
RELEASE_CHECK="$TEST_ROOT/repo/scripts/dev/release-check.sh"
REAL_PYTHON=$(command -v python3)
PASS=0
export TEST_ROOT REAL_PYTHON

cleanup() {
    [[ "$TEST_ROOT" == /tmp/zlink-release-check-test.* ]] && rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

pass() {
    PASS=$((PASS + 1))
    printf 'ok %d - %s\n' "$PASS" "$1"
}

assert_file_contains() {
    local file=$1 pattern=$2
    grep -Eq -- "$pattern" "$file" || fail "$file 에서 패턴을 찾지 못함: $pattern"
}

assert_success() {
    local description=$1
    shift
    if ! "$@" >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err"; then
        sed -n '1,200p' "$TEST_ROOT/last.out" >&2
        sed -n '1,200p' "$TEST_ROOT/last.err" >&2
        fail "$description"
    fi
}

assert_failure() {
    local description=$1 expected=$2 actual
    shift 2
    set +e
    "$@" >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err"
    actual=$?
    set -e
    [[ "$actual" -eq "$expected" ]] || {
        sed -n '1,200p' "$TEST_ROOT/last.out" >&2
        sed -n '1,200p' "$TEST_ROOT/last.err" >&2
        fail "$description: exit $actual (expected $expected)"
    }
}

write_node_package() {
    local relative=$1 name=$2 directory=$3
    mkdir -p "$TEST_ROOT/repo/$(dirname "$relative")"
    cat >"$TEST_ROOT/repo/$relative" <<EOF
{
  "name": "$name",
  "version": "1.2.3",
  "repository": {
    "type": "git",
    "url": "https://github.com/zlink-systems/zlink.git",
    "directory": "$directory"
  },
  "homepage": "https://zlink.systems/"
}
EOF
}

write_cpp_metadata() {
    local target=$1 port=$2 prefix=$3 tag=$4 asset=$5
    mkdir -p "$TEST_ROOT/repo/vcpkg/ports/$port" "$TEST_ROOT/repo/$prefix"
    cat >"$TEST_ROOT/repo/vcpkg/ports/$port/vcpkg.json" <<EOF
{"name":"$port","version":"1.2.3"}
EOF
    cat >"$TEST_ROOT/repo/vcpkg/ports/$port/portfile.cmake" <<EOF
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/$tag/$asset"
    SHA512 11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
)
EOF
    if [[ "$target" != core ]]; then
        cat >"$TEST_ROOT/repo/$prefix/conanfile.py" <<EOF
class Recipe:
    version = "1.2.3"
EOF
    else
        printf 'class Recipe: pass\n' >"$TEST_ROOT/repo/$prefix/conanfile.py"
    fi
    cat >"$TEST_ROOT/repo/$prefix/conandata.yml" <<EOF
sources:
  "1.2.3":
    url: "https://github.com/zlink-systems/zlink/releases/download/$tag/$asset"
    sha256: "2222222222222222222222222222222222222222222222222222222222222222"
EOF
}

make_fake_commands() {
    mkdir -p "$TEST_ROOT/fake-bin"
    cat >"$TEST_ROOT/fake-bin/python3" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == */scripts/local-package/sync-version.py ]]; then
    printf '%s\n' "$*" >>"$TEST_ROOT/sync-calls.log"
    [[ ! -f "$TEST_ROOT/repo/sync-fail" ]]
    exit
fi
exec "$REAL_PYTHON" "$@"
EOF
    cat >"$TEST_ROOT/fake-bin/network-command" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s %s\n' "${0##*/}" "$*" >>"$TEST_ROOT/network-calls.log"
exit 99
EOF
    chmod +x "$TEST_ROOT/fake-bin/python3" "$TEST_ROOT/fake-bin/network-command"
    local command
    for command in curl gh npm dotnet gradle conan; do
        ln -s network-command "$TEST_ROOT/fake-bin/$command"
    done
}

make_fixture() {
    local package project package_id
    mkdir -p \
        "$TEST_ROOT/repo/scripts/dev" \
        "$TEST_ROOT/repo/scripts/local-package" \
        "$TEST_ROOT/repo/doc/building/release-notes" \
        "$TEST_ROOT/repo/bindings/cpp" \
        "$TEST_ROOT/repo/bindings/dotnet/src/Zlink" \
        "$TEST_ROOT/repo/bindings/java/scripts" \
        "$TEST_ROOT/repo/bindings/java/codec/zlink-ext-netty" \
        "$TEST_ROOT/repo/bindings/node" \
        "$TEST_ROOT/repo/framework/languages/dotnet/src" \
        "$TEST_ROOT/repo/framework/languages/java/scripts"
    cp "$SOURCE_ROOT/scripts/dev/release-check.sh" "$RELEASE_CHECK"
    chmod +x "$RELEASE_CHECK"
    printf '# fixture\n' >"$TEST_ROOT/repo/scripts/local-package/sync-version.py"
    cat >"$TEST_ROOT/repo/VERSION" <<'EOF'
LIBZLINK_VERSION_MAJOR=1
LIBZLINK_VERSION_MINOR=2
LIBZLINK_VERSION_PATCH=3
LIBZLINK_VERSION=1.2.3
EOF
    printf '# core 1.2.3\n' >"$TEST_ROOT/repo/doc/building/release-notes/core-1.2.3.ko.md"
    printf '# core 1.2.3\n' >"$TEST_ROOT/repo/doc/building/release-notes/core-1.2.3.md"
    for language in cpp node java dotnet; do
        mkdir -p "$TEST_ROOT/repo/bindings/$language" "$TEST_ROOT/repo/framework/languages/$language"
        printf 'ZLINK_BINDING_VERSION=1.2.3\n' >"$TEST_ROOT/repo/bindings/$language/VERSION"
        printf 'ZLINK_FRAMEWORK_VERSION=1.2.3\n' >"$TEST_ROOT/repo/framework/languages/$language/VERSION"
        for target in bindings framework; do
            printf '# %s %s 1.2.3\n' "$target" "$language" \
                >"$TEST_ROOT/repo/doc/building/release-notes/$target-$language-1.2.3.ko.md"
            printf '# %s %s 1.2.3\n' "$target" "$language" \
                >"$TEST_ROOT/repo/doc/building/release-notes/$target-$language-1.2.3.md"
        done
    done

    write_node_package bindings/node/package.json @zlink-systems/zlink bindings/node
    for package in stream-wire stream-connector framework framework-codec-protobuf \
        framework-codec-msgpack framework-locations-redis http-client nestjs; do
        write_node_package \
            "framework/languages/node/packages/$package/package.json" \
            "@zlink-systems/$package" \
            "framework/languages/node/packages/$package"
    done

    cat >"$TEST_ROOT/repo/bindings/dotnet/src/Zlink/Zlink.csproj" <<'EOF'
<Project><PropertyGroup>
<ZLinkBindingVersionFile>../../VERSION</ZLinkBindingVersionFile>
<Version>$([System.IO.File]::ReadAllText('$(ZLinkBindingVersionFile)').Trim().Replace('ZLINK_BINDING_VERSION=', ''))</Version>
<PackageId>Zlink</PackageId><Description>binding</Description>
</PropertyGroup></Project>
EOF
    cat >"$TEST_ROOT/repo/bindings/dotnet/Directory.Build.targets" <<'EOF'
<Project><PropertyGroup>
<Authors>zlink</Authors><PackageLicenseExpression>MPL-2.0</PackageLicenseExpression>
<PackageProjectUrl>https://github.com/zlink-systems/zlink</PackageProjectUrl>
<RepositoryUrl>https://github.com/zlink-systems/zlink</RepositoryUrl><RepositoryType>git</RepositoryType>
<PackageReadmeFile>README.md</PackageReadmeFile><PublishRepositoryUrl>true</PublishRepositoryUrl>
</PropertyGroup></Project>
EOF
    cat >"$TEST_ROOT/repo/framework/languages/dotnet/Directory.Build.props" <<'EOF'
<Project><PropertyGroup>
<ZLinkFrameworkVersionFile>VERSION</ZLinkFrameworkVersionFile>
<Version>$([System.IO.File]::ReadAllText('$(ZLinkFrameworkVersionFile)').Trim().Replace('ZLINK_FRAMEWORK_VERSION=', ''))</Version>
<PackageLicenseFile>LICENSE</PackageLicenseFile>
<PackageProjectUrl>https://github.com/zlink-systems/zlink</PackageProjectUrl>
</PropertyGroup></Project>
EOF
    for project in Systems.Zlink.Stream.Connector Zlink.Framework.AspNetCore \
        Zlink.Framework.Codecs.MessagePack Zlink.Framework.Codecs.Protobuf \
        Zlink.Framework.Contracts Zlink.Framework.Locations.Redis \
        Zlink.Framework.Provider.Abstractions Zlink.Framework Zlink.HttpClient; do
        mkdir -p "$TEST_ROOT/repo/framework/languages/dotnet/src/$project"
        package_id=""
        [[ "$project" != Systems.Zlink.Stream.Connector ]] \
            || package_id='<PackageId>Zlink.Stream.Connector</PackageId>'
        cat >"$TEST_ROOT/repo/framework/languages/dotnet/src/$project/$project.csproj" <<EOF
<Project><PropertyGroup><IsPackable>true</IsPackable><Description>$project</Description>$package_id</PropertyGroup></Project>
EOF
    done

    cat >"$TEST_ROOT/repo/bindings/java/build.gradle" <<'EOF'
group = 'systems.zlink'
version = '1.2.3'
url = 'https://github.com/zlink-systems/zlink'
licenses { }
developers { }
scm { }
withSourcesJar()
withJavadocJar()
name = 'centralStaging'
EOF
    cat >"$TEST_ROOT/repo/bindings/java/scripts/upload-central-bundle.sh" <<'EOF'
java_root="bindings/java"
version="$(sed -n 's/^ZLINK_BINDING_VERSION=//p' "$java_root/VERSION")"
bundle="zlink-java-$version-central-bundle.zip"
EOF
    cat >"$TEST_ROOT/repo/bindings/java/codec/zlink-ext-netty/build.gradle" <<'EOF'
plugins { id 'maven-publish' }
description = 'Netty codec'
EOF
    cat >"$TEST_ROOT/repo/framework/languages/java/build.gradle.kts" <<'EOF'
group = "systems.zlink"
version = providers.fileContents(layout.projectDirectory.file("VERSION"))
url.set("https://github.com/zlink-systems/zlink")
licenses { }
developers { }
scm { }
withSourcesJar()
withJavadocJar()
name = "centralStaging"
EOF
    cat >"$TEST_ROOT/repo/framework/languages/java/scripts/upload-central-bundle.sh" <<'EOF'
java_root="framework/languages/java"
version="$(sed -n 's/^ZLINK_FRAMEWORK_VERSION=//p' "$java_root/VERSION")"
bundle="zlink-framework-java-$version-central-bundle.zip"
EOF
    for package in zlink-framework-binding-internal zlink-framework-codec-msgpack \
        zlink-framework-codec-protobuf zlink-framework-core zlink-framework-json-internal \
        zlink-framework-kotlin zlink-framework-locations-redis \
        zlink-framework-provider-abstractions zlink-framework-spring-boot-starter \
        zlink-framework-testkit zlink-http-client zlink-http-client-kotlin \
        zlink-stream-connector; do
        mkdir -p "$TEST_ROOT/repo/framework/languages/java/$package"
        cat >"$TEST_ROOT/repo/framework/languages/java/$package/build.gradle.kts" <<EOF
plugins { \`maven-publish\` }
description = "$package"
EOF
    done

    write_cpp_metadata core zlink core/packaging/conan core/v1.2.3 zlink-1.2.3-source.tar.gz
    write_cpp_metadata bindings zlink-cpp bindings/cpp/packaging/conan cpp/v1.2.3 zlink-cpp-1.2.3.tar.gz
    write_cpp_metadata framework zlink-framework framework/languages/cpp/packaging/conan \
        framework-cpp/v1.2.3 zlink-framework-cpp-1.2.3.tar.gz
}

release_check() {
    env PATH="$TEST_ROOT/fake-bin:$PATH" "$RELEASE_CHECK" "$@"
}

mkdir -p "$TEST_ROOT"
make_fake_commands
make_fixture

assert_success 'Core 정상 검사 실패' release_check core 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '릴리스 사전 검사 통과: core 1.2.3'
for target in bindings framework; do
    for language in cpp node java dotnet; do
        assert_success "$target/$language 정상 검사 실패" release_check "$target" "$language" 1.2.3
        assert_file_contains "$TEST_ROOT/last.out" "릴리스 사전 검사 통과: $target/$language 1.2.3"
    done
done
[[ $(wc -l <"$TEST_ROOT/sync-calls.log") -eq 9 ]] || fail 'sync-version.py --check 호출 수가 다름'
[[ ! -e "$TEST_ROOT/network-calls.log" ]] || fail '검사 중 네트워크 명령을 호출함'
pass 'Core와 언어별 binding/framework 버전·노트·메타데이터·배포 표를 오프라인으로 검사한다'

assert_success 'dry-run 검사 실패' release_check framework --dry-run dotnet 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^dry-run: 읽기 전용 검사를 동일하게 수행'
assert_file_contains "$TEST_ROOT/last.out" '^\| 1 \| framework-dotnet/v1.2.3 \| nuget.org \(9개\) \|$'
pass '전역 --dry-run도 같은 읽기 전용 검사와 배포 대상 표를 제공한다'

assert_failure '인자 누락 거부 실패' 2 release_check bindings
assert_failure '언어 누락 거부 실패' 2 release_check framework 1.2.3
assert_failure '잘못된 언어 거부 실패' 2 release_check bindings ruby 1.2.3
assert_failure '잘못된 대상 거부 실패' 2 release_check unknown 1.2.3
assert_failure '잘못된 버전 거부 실패' 2 release_check core v1.2.3
pass '잘못된 인자는 exit 2로 거부한다'

rm "$TEST_ROOT/repo/doc/building/release-notes/bindings-java-1.2.3.md"
assert_failure '릴리스 노트 누락 검출 실패' 1 release_check bindings java 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 릴리스 노트 \| bindings/java \| FAIL \|'
assert_file_contains "$TEST_ROOT/last.err" '^재개 명령:'
printf '# bindings java 1.2.3\n' >"$TEST_ROOT/repo/doc/building/release-notes/bindings-java-1.2.3.md"
pass '선택 언어의 두 릴리스 노트 중 하나라도 없으면 실패하고 재개 명령을 출력한다'

touch "$TEST_ROOT/repo/sync-fail"
assert_failure '버전 동기화 실패 전파 실패' 1 release_check core 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 버전 \| 전체 \| FAIL \| sync-version.py --check \|$'
rm "$TEST_ROOT/repo/sync-fail"
pass 'sync-version.py --check 실패를 집계한다'

cp "$TEST_ROOT/repo/bindings/node/package.json" "$TEST_ROOT/node-package.backup"
sed -i 's#https://zlink.systems/#https://wrong.example/#' "$TEST_ROOT/repo/bindings/node/package.json"
assert_success '선택하지 않은 언어 메타데이터가 검사를 막음' release_check bindings java 1.2.3
assert_failure 'npm 메타데이터 실패 검출 실패' 1 release_check bindings node 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 패키지 메타데이터 \| npm \| FAIL \|'
mv "$TEST_ROOT/node-package.backup" "$TEST_ROOT/repo/bindings/node/package.json"
pass '선택 언어 metadata만 검사하고 선택한 npm의 repository/homepage 오류를 거부한다'

cp "$TEST_ROOT/repo/bindings/dotnet/Directory.Build.targets" "$TEST_ROOT/dotnet-targets.backup"
sed -i 's#<RepositoryUrl>#<MissingRepositoryUrl>#' "$TEST_ROOT/repo/bindings/dotnet/Directory.Build.targets"
assert_failure 'NuGet 메타데이터 실패 검출 실패' 1 release_check bindings dotnet 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 패키지 메타데이터 \| NuGet \| FAIL \|'
mv "$TEST_ROOT/dotnet-targets.backup" "$TEST_ROOT/repo/bindings/dotnet/Directory.Build.targets"
pass 'NuGet 필수 패키지 메타데이터 오류를 거부한다'

cp "$TEST_ROOT/repo/framework/languages/java/scripts/upload-central-bundle.sh" "$TEST_ROOT/central-script.backup"
sed -i 's/ZLINK_FRAMEWORK_VERSION/LITERAL_VERSION/' \
    "$TEST_ROOT/repo/framework/languages/java/scripts/upload-central-bundle.sh"
assert_failure 'Maven bundle 버전 실패 검출 실패' 1 release_check framework java 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 패키지 메타데이터 \| Maven \| FAIL \|'
mv "$TEST_ROOT/central-script.backup" \
    "$TEST_ROOT/repo/framework/languages/java/scripts/upload-central-bundle.sh"
pass 'Maven Central bundle 스크립트의 버전 파일 연결 누락을 거부한다'

cp "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake" "$TEST_ROOT/portfile.backup"
sed -i 's/SHA512 [0-9a-f]*/SHA512 short/' \
    "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake"
assert_failure 'vcpkg SHA 실패 검출 실패' 1 release_check framework cpp 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 패키지 메타데이터 \| Conan·vcpkg \| FAIL \|'
mv "$TEST_ROOT/portfile.backup" "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake"
pass 'vcpkg port 버전과 SHA 형식을 검사한다'

cp "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" "$TEST_ROOT/conandata.backup"
sed -i 's/sha256: "[0-9a-f]*"/sha256: "short"/' \
    "$TEST_ROOT/repo/core/packaging/conan/conandata.yml"
assert_failure 'Conan SHA 실패 검출 실패' 1 release_check core 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 패키지 메타데이터 \| Conan·vcpkg \| FAIL \|'
mv "$TEST_ROOT/conandata.backup" "$TEST_ROOT/repo/core/packaging/conan/conandata.yml"
pass 'Conan recipe 버전·자산 URL·SHA 형식을 검사한다'

printf '1..%d\n' "$PASS"
