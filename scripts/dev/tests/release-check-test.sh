#!/usr/bin/env bash

set -euo pipefail

TEST_ROOT=/tmp/zlink-release-check-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
RELEASE_CHECK="$TEST_ROOT/repo/scripts/dev/release-check.sh"
REAL_PYTHON=$(command -v python3)
PASS=0
export TEST_ROOT REAL_PYTHON

HTTP_SERVER_PID=""

cleanup() {
    [[ -n "$HTTP_SERVER_PID" ]] && kill "$HTTP_SERVER_PID" >/dev/null 2>&1
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

### 쓰기 모드 (Task 1, 이슈 #378): vcpkg port·Conan recipe 자동화.
#
# --write는 GitHub에서 실제 바이트를 받는다. 오프라인 재현을 위해 로컬
# HTTP 서버가 고정된 fixture 바이트를 내주고, ZLINK_RELEASE_CHECK_GITHUB_BASE_URL로
# release-check.sh가 그 서버를 보게 한다(write_cpp()만 이 변수를 읽고, 실제
# 실행에서는 설정하지 않는다).

HTTP_ROOT="$TEST_ROOT/http-root"
mkdir -p "$HTTP_ROOT/zlink-systems/zlink/releases/download" "$HTTP_ROOT/zlink-systems/zlink/archive"
HTTP_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
(cd "$HTTP_ROOT" && exec python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 >"$TEST_ROOT/http-server.log" 2>&1) &
HTTP_SERVER_PID=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/" && break
    sleep 0.1
done

release_check_write() {
    env PATH="$TEST_ROOT/fake-bin:$PATH" \
        ZLINK_RELEASE_CHECK_GITHUB_BASE_URL="http://127.0.0.1:$HTTP_PORT" \
        "$RELEASE_CHECK" "$@"
}

# --- distfile 형태(vcpkg_download_distfile): framework cpp, 값이 이미 맞음 ---
fw_asset_dir="$HTTP_ROOT/zlink-systems/zlink/releases/download/framework-cpp/v1.2.3"
mkdir -p "$fw_asset_dir"
printf 'framework-cpp fixture asset\n' >"$fw_asset_dir/zlink-framework-cpp-1.2.3.tar.gz"
fw_sha256=$(sha256sum "$fw_asset_dir/zlink-framework-cpp-1.2.3.tar.gz" | awk '{print $1}')
fw_sha512=$(sha512sum "$fw_asset_dir/zlink-framework-cpp-1.2.3.tar.gz" | awk '{print $1}')

assert_success 'framework cpp 쓰기 실패' release_check_write --write framework cpp 1.2.3
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake" "SHA512 ${fw_sha512}\$"
assert_file_contains "$TEST_ROOT/repo/framework/languages/cpp/packaging/conan/conandata.yml" \
    "sha256: \"${fw_sha256}\""
pass 'distfile 형태(vcpkg_download_distfile) 포트는 릴리스 자산 하나로 vcpkg SHA512와 Conan sha256을 함께 채운다'

assert_success '재실행 무변경 확인 실패' release_check_write --write framework cpp 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^변경 없음: vcpkg/ports/zlink-framework/portfile.cmake$'
assert_file_contains "$TEST_ROOT/last.out" '^변경 없음: framework/languages/cpp/packaging/conan/conandata.yml$'
pass '값이 이미 맞으면 재실행해도 파일을 다시 쓰지 않는다(재실행 안전)'

# --- 문자 그대로 박힌 오래된 버전·해시 복구: bindings cpp (issue #378 재현) ---
sed -i 's/1\.2\.3/1.2.2/g' \
    "$TEST_ROOT/repo/vcpkg/ports/zlink-cpp/vcpkg.json" \
    "$TEST_ROOT/repo/vcpkg/ports/zlink-cpp/portfile.cmake" \
    "$TEST_ROOT/repo/bindings/cpp/packaging/conan/conanfile.py" \
    "$TEST_ROOT/repo/bindings/cpp/packaging/conan/conandata.yml"
assert_failure '갱신 전 검사가 실패해야 함' 1 release_check bindings cpp 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^\| 패키지 메타데이터 \| Conan·vcpkg \| FAIL \|'

cpp_asset_dir="$HTTP_ROOT/zlink-systems/zlink/releases/download/cpp/v1.2.3"
mkdir -p "$cpp_asset_dir"
printf 'bindings cpp fixture asset\n' >"$cpp_asset_dir/zlink-cpp-1.2.3.tar.gz"
cpp_sha256=$(sha256sum "$cpp_asset_dir/zlink-cpp-1.2.3.tar.gz" | awk '{print $1}')
cpp_sha512=$(sha512sum "$cpp_asset_dir/zlink-cpp-1.2.3.tar.gz" | awk '{print $1}')

assert_success 'bindings cpp 쓰기 실패' release_check_write --write bindings cpp 1.2.3
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink-cpp/vcpkg.json" '"version": "1.2.3"'
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink-cpp/portfile.cmake" 'cpp/v1\.2\.3/zlink-cpp-1\.2\.3\.tar\.gz'
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink-cpp/portfile.cmake" "SHA512 ${cpp_sha512}\$"
assert_file_contains "$TEST_ROOT/repo/bindings/cpp/packaging/conan/conanfile.py" 'version = "1.2.3"'
assert_file_contains "$TEST_ROOT/repo/bindings/cpp/packaging/conan/conandata.yml" '"1\.2\.3":'
assert_file_contains "$TEST_ROOT/repo/bindings/cpp/packaging/conan/conandata.yml" "sha256: \"${cpp_sha256}\""

assert_success '되살린 뒤 검사 실패' release_check bindings cpp 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '릴리스 사전 검사 통과: bindings/cpp 1.2.3'
pass '오래된 버전·해시가 박힌 vcpkg port·Conan recipe를 --write가 최신 릴리스 자산에 맞춰 복구한다(검사 FAIL에서 PASS로)'

# --- dry-run: 값을 계산해 보여주기만 하고 파일은 바꾸지 않는다 ---
sed -i 's/1\.2\.3/1.2.2/g' \
    "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/vcpkg.json" \
    "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake" \
    "$TEST_ROOT/repo/framework/languages/cpp/packaging/conan/conanfile.py" \
    "$TEST_ROOT/repo/framework/languages/cpp/packaging/conan/conandata.yml"
before_portfile=$(cat "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake")
assert_success 'framework cpp dry-run 쓰기 실패' release_check_write --write --dry-run framework cpp 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^dry-run: 갱신 예정: vcpkg/ports/zlink-framework/portfile\.cmake$'
after_portfile=$(cat "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake")
[[ "$before_portfile" == "$after_portfile" ]] || fail '--dry-run인데 portfile.cmake가 실제로 바뀜'
pass '--write --dry-run은 갱신 내용을 계산해 보여주기만 하고 파일은 바꾸지 않는다'

# --- github 자동 아카이브 형태(vcpkg_from_github): core, 자산과 다른 바이트 ---
cat >"$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" <<'EOF'
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zlink-systems/zlink
    REF core/v1.2.2
    SHA512 1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
    HEAD_REF main
)
EOF
sed -i 's/"version":"1.2.3"/"version":"1.2.2"/' "$TEST_ROOT/repo/vcpkg/ports/zlink/vcpkg.json"
sed -i 's/"1\.2\.3":/"1.2.2":/' "$TEST_ROOT/repo/core/packaging/conan/conandata.yml"

mkdir -p "$HTTP_ROOT/zlink-systems/zlink/archive/core" \
    "$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3"
printf 'core github-archive fixture bytes\n' \
    >"$HTTP_ROOT/zlink-systems/zlink/archive/core/v1.2.3.tar.gz"
printf 'core release-asset fixture bytes (다른 내용)\n' \
    >"$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/zlink-1.2.3-source.tar.gz"
core_archive_sha512=$(sha512sum "$HTTP_ROOT/zlink-systems/zlink/archive/core/v1.2.3.tar.gz" | awk '{print $1}')
core_asset_sha256=$(sha256sum \
    "$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/zlink-1.2.3-source.tar.gz" | awk '{print $1}')

assert_success 'core(vcpkg_from_github) 쓰기 실패' release_check_write --write core 1.2.3
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/vcpkg.json" '"version": "1.2.3"'
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" '^[[:space:]]*REF core/v1\.2\.3$'
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" "SHA512 ${core_archive_sha512}\$"
assert_file_contains "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" '"1\.2\.3":'
assert_file_contains "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" "sha256: \"${core_asset_sha256}\""
grep -q -- "$core_archive_sha512" "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" \
    && fail 'vcpkg SHA512이 실수로 conandata에 들어감(자산과 아카이브를 구분하지 못함)'
pass 'vcpkg_from_github 포트는 GitHub 자동 아카이브의 SHA512를, Conan sha256은 릴리스 자산에서 따로 계산한다(서로 다른 바이트, 서로 다른 해시)'

# --- 판정 불가: vcpkg_from_github와 vcpkg_download_distfile이 함께 있으면 거부 ---
cat >>"$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" <<'EOF'

vcpkg_download_distfile(ARCHIVE
    URLS "https://example.invalid/x.tar.gz"
    SHA512 2222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222
)
EOF
assert_failure '형태 판정 모호 거부 실패' 1 release_check_write --write core 1.2.3
assert_file_contains "$TEST_ROOT/last.err" '판정할 수 없음'
pass 'vcpkg_from_github와 vcpkg_download_distfile이 함께 있으면 어느 해시를 계산할지 정하지 않고 실패한다'

# --- 릴리스 자산이 없으면 실패하고 아무 파일도 바꾸지 않는다 ---
before_hash=$(sha256sum "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake" | awk '{print $1}')
assert_failure '존재하지 않는 릴리스 자산 다운로드 실패 검출 실패' 1 release_check_write --write framework cpp 9.9.9
assert_file_contains "$TEST_ROOT/last.err" '다운로드 실패'
after_hash=$(sha256sum "$TEST_ROOT/repo/vcpkg/ports/zlink-framework/portfile.cmake" | awk '{print $1}')
[[ "$before_hash" == "$after_hash" ]] || fail '다운로드 실패에도 portfile.cmake가 바뀜(부분 쓰기 금지 위반)'
pass '릴리스 자산이 없으면 값을 추측하지 않고 실패하며 아무 파일도 바꾸지 않는다'

# --- 대상 검증: --write는 core 또는 bindings/framework의 cpp에만 있다 ---
assert_failure '--write node 거부 실패' 2 release_check_write --write bindings node 1.2.3
assert_failure '--write dotnet 거부 실패' 2 release_check_write --write framework dotnet 1.2.3
pass '--write는 npm·NuGet·Maven 언어를 거부한다(vcpkg·Conan 레시피만 이 자동화의 대상)'

# --- 혼합 형태(issue #419): vcpkg_from_github 소스 폴백 + 플랫폼별
# vcpkg_download_distfile(SHA512가 리터럴이 아니라 if/elseif로 고르는 변수).
# PR #413이 core port를 이 모양으로 만들었다; 여기서는 linux-x64/linux-arm64
# 두 플랫폼만으로 같은 메커니즘을 확인한다.
cat >"$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" <<'EOF'
set(ZLINK_RELEASE_TAG "core/v1.2.2")
set(ZLINK_RELEASE_VERSION "1.2.2")
set(ZLINK_ARCHIVE_PLATFORM "")
if(VCPKG_TARGET_IS_LINUX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(ZLINK_ARCHIVE_PLATFORM "linux-x64")
        set(ZLINK_ARCHIVE_SHA512 "1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(ZLINK_ARCHIVE_PLATFORM "linux-arm64")
        set(ZLINK_ARCHIVE_SHA512 "2222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222")
    endif()
endif()
if(ZLINK_ARCHIVE_PLATFORM)
    vcpkg_download_distfile(ZLINK_ARCHIVE
        URLS "https://github.com/zlink-systems/zlink/releases/download/${ZLINK_RELEASE_TAG}/libzlink-${ZLINK_ARCHIVE_PLATFORM}.tar.gz"
        FILENAME "libzlink-${ZLINK_RELEASE_VERSION}-${ZLINK_ARCHIVE_PLATFORM}.tar.gz"
        SHA512 "${ZLINK_ARCHIVE_SHA512}"
    )
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO zlink-systems/zlink
        REF "${ZLINK_RELEASE_TAG}"
        SHA512 3333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333
        HEAD_REF main
    )
endif()
EOF
sed -i 's/"version":"1.2.3"/"version":"1.2.2"/' "$TEST_ROOT/repo/vcpkg/ports/zlink/vcpkg.json"
cat >"$TEST_ROOT/repo/core/packaging/conan/conandata.yml" <<'EOF'
binaries:
  "1.2.2":
    linux-x64:
      url: "https://github.com/zlink-systems/zlink/releases/download/core/v1.2.2/libzlink-linux-x64.tar.gz"
      sha256: "4444444444444444444444444444444444444444444444444444444444444444"
    linux-arm64:
      url: "https://github.com/zlink-systems/zlink/releases/download/core/v1.2.2/libzlink-linux-arm64.tar.gz"
      sha256: "5555555555555555555555555555555555555555555555555555555555555555"

sources:
  "1.2.2":
    url: "https://github.com/zlink-systems/zlink/releases/download/core/v1.2.2/zlink-1.2.2-source.tar.gz"
    sha256: "6666666666666666666666666666666666666666666666666666666666666666"
EOF

mkdir -p "$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3"
printf 'core linux-x64 archive fixture bytes\n' \
    >"$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/libzlink-linux-x64.tar.gz"
printf 'core linux-arm64 archive fixture bytes\n' \
    >"$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/libzlink-linux-arm64.tar.gz"
linux_x64_sha512=$(sha512sum "$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/libzlink-linux-x64.tar.gz" | awk '{print $1}')
linux_x64_sha256=$(sha256sum "$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/libzlink-linux-x64.tar.gz" | awk '{print $1}')
linux_arm64_sha512=$(sha512sum "$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/libzlink-linux-arm64.tar.gz" | awk '{print $1}')
linux_arm64_sha256=$(sha256sum "$HTTP_ROOT/zlink-systems/zlink/releases/download/core/v1.2.3/libzlink-linux-arm64.tar.gz" | awk '{print $1}')
# core/v1.2.3의 GitHub 자동 아카이브·릴리스 소스 자산은 앞의 vcpkg_from_github
# 테스트가 이미 만들어 두었다(archive/core/v1.2.3.tar.gz,
# releases/download/core/v1.2.3/zlink-1.2.3-source.tar.gz).

assert_success '혼합 형태(github+플랫폼별 distfile) 쓰기 실패' release_check_write --write core 1.2.3
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" '^set\(ZLINK_RELEASE_TAG "core/v1\.2\.3"\)$'
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" '^set\(ZLINK_RELEASE_VERSION "1\.2\.3"\)$'
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" "SHA512 ${core_archive_sha512}\$"
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" "ZLINK_ARCHIVE_SHA512 \"${linux_x64_sha512}\"\\)\$"
assert_file_contains "$TEST_ROOT/repo/vcpkg/ports/zlink/portfile.cmake" "ZLINK_ARCHIVE_SHA512 \"${linux_arm64_sha512}\"\\)\$"
assert_file_contains "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" '"1\.2\.3":'
assert_file_contains "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" "sha256: \"${linux_x64_sha256}\""
assert_file_contains "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" "sha256: \"${linux_arm64_sha256}\""
assert_file_contains "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" "sha256: \"${core_asset_sha256}\""
# 이전 버전(1.2.2)의 binaries:/sources: 항목은 sources:가 다른 모든 버전에 대해
# 그러듣이 과거 기록으로 남는다 -- 삭제 대상이 아니다.
assert_file_contains "$TEST_ROOT/repo/core/packaging/conan/conandata.yml" '"1\.2\.2":'

assert_success '혼합 형태 재실행 무변경 확인 실패' release_check_write --write core 1.2.3
assert_file_contains "$TEST_ROOT/last.out" '^변경 없음: vcpkg/ports/zlink/portfile\.cmake$'
assert_file_contains "$TEST_ROOT/last.out" '^변경 없음: core/packaging/conan/conandata\.yml$'
pass 'vcpkg_from_github 폴백과 플랫폼별 vcpkg_download_distfile(변수 SHA512)이 함께 있어도 거부하지 않고, 각자의 해시(GitHub 아카이브·플랫폼별 아카이브·릴리스 소스 자산)를 읽어 portfile의 여러 SHA512와 conandata.yml의 binaries:·sources: 항목을 모두 갱신하며 재실행은 무변경이다'

kill "$HTTP_SERVER_PID" >/dev/null 2>&1 || true
wait "$HTTP_SERVER_PID" 2>/dev/null || true
HTTP_SERVER_PID=""

printf '1..%d\n' "$PASS"
