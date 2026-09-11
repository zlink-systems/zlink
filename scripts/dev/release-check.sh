#!/usr/bin/env bash

set -euo pipefail

DRY_RUN=0
RELEASE_TARGET=""
RELEASE_LANGUAGE=""
RELEASE_VERSION=""
RESUME_COMMAND=""
FAILURES=0
CHECK_AREAS=()
CHECK_TARGETS=()
CHECK_RESULTS=()
CHECK_DETAILS=()

usage() {
    cat <<'EOF'
사용법:
  release-check.sh [--dry-run] core <X.Y.Z>
  release-check.sh [--dry-run] bindings <cpp|node|java|dotnet> <X.Y.Z>
  release-check.sh [--dry-run] framework <cpp|node|java|dotnet> <X.Y.Z>
EOF
}

quote_command() {
    local quoted=() value
    for value in "$@"; do
        printf -v value '%q' "$value"
        quoted+=("$value")
    done
    printf '%s' "${quoted[*]}"
}

die() {
    local code=$1
    shift
    printf '오류: %s\n' "$*" >&2
    [[ -n "$RELEASE_TARGET" ]] && printf '  대상: %s\n' "$RELEASE_TARGET" >&2
    [[ -n "$RELEASE_LANGUAGE" ]] && printf '  언어: %s\n' "$RELEASE_LANGUAGE" >&2
    [[ -n "$RELEASE_VERSION" ]] && printf '  버전: %s\n' "$RELEASE_VERSION" >&2
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit "$code"
}

on_error() {
    local code=$1 line=$2
    trap - ERR
    printf '오류: %s행에서 명령이 실패했습니다 (exit %s).\n' "$line" "$code" >&2
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit "$code"
}
trap 'on_error $? $LINENO' ERR

require_command() {
    command -v "$1" >/dev/null 2>&1 || die 1 "필요한 명령을 찾을 수 없습니다: $1"
}

repo_root() {
    local root
    root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
    [[ -f "$root/scripts/local-package/sync-version.py" ]] \
        || die 1 "저장소 루트를 확인할 수 없습니다: $root"
    printf '%s\n' "$root"
}

record_check() {
    local area=$1 target=$2 result=$3 detail=$4
    CHECK_AREAS+=("$area")
    CHECK_TARGETS+=("$target")
    CHECK_RESULTS+=("$result")
    CHECK_DETAILS+=("$detail")
    if [[ "$result" == FAIL ]]; then
        FAILURES=$((FAILURES + 1))
    fi
}

check_version_sync() {
    local root=$1 version_file version_field declared
    case "$RELEASE_TARGET" in
        core) version_file=VERSION; version_field=LIBZLINK_VERSION ;;
        bindings) version_file="bindings/$RELEASE_LANGUAGE/VERSION"; version_field=ZLINK_BINDING_VERSION ;;
        framework) version_file="framework/languages/$RELEASE_LANGUAGE/VERSION"; version_field=ZLINK_FRAMEWORK_VERSION ;;
    esac

    if python3 "$root/scripts/local-package/sync-version.py" --check >/dev/null; then
        record_check 버전 전체 PASS 'sync-version.py --check'
    else
        record_check 버전 전체 FAIL 'sync-version.py --check'
    fi

    declared=$(sed -n "s/^${version_field}=//p" "$root/$version_file")
    if [[ "$declared" == "$RELEASE_VERSION" ]]; then
        record_check 버전 "${RELEASE_TARGET}${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" PASS "$version_file=$RELEASE_VERSION"
    else
        record_check 버전 "${RELEASE_TARGET}${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" FAIL "$version_file=${declared:-<없음>} (요청 $RELEASE_VERSION)"
    fi
}

check_release_notes() {
    local root=$1 base missing=()
    if [[ "$RELEASE_TARGET" == core ]]; then
        base="$root/doc/building/release-notes/core-$RELEASE_VERSION"
    else
        base="$root/doc/building/release-notes/$RELEASE_TARGET-$RELEASE_LANGUAGE-$RELEASE_VERSION"
    fi
    [[ -f "$base.ko.md" ]] || missing+=("${base#"$root/"}.ko.md")
    [[ -f "$base.md" ]] || missing+=("${base#"$root/"}.md")
    if ((${#missing[@]} == 0)); then
        record_check '릴리스 노트' "${RELEASE_TARGET}${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" PASS "${base#"$root/"}.{ko,}.md"
    else
        record_check '릴리스 노트' "${RELEASE_TARGET}${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" FAIL "누락: ${missing[*]}"
    fi
}

validate_metadata() {
    local root=$1 mode=$2
    python3 - "$root" "$RELEASE_TARGET" "$RELEASE_LANGUAGE" "$RELEASE_VERSION" "$mode" <<'PY'
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
target = sys.argv[2]
language = sys.argv[3]
version = sys.argv[4]
mode = sys.argv[5]
errors = []


def error(message):
    errors.append(message)


def text(relative):
    path = root / relative
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        error(f"{relative}: 읽기 실패: {exc}")
        return ""


def json_file(relative):
    source = text(relative)
    if not source:
        return {}
    try:
        value = json.loads(source)
    except json.JSONDecodeError as exc:
        error(f"{relative}: JSON 오류: {exc}")
        return {}
    if not isinstance(value, dict):
        error(f"{relative}: 최상위 값이 객체가 아님")
        return {}
    return value


def contains(relative, *needles):
    source = text(relative)
    for needle in needles:
        if needle not in source:
            error(f"{relative}: 필수 메타데이터 누락: {needle}")


def npm_package(relative, expected_directory):
    data = json_file(relative)
    if data.get("version") != version:
        error(f"{relative}: version={data.get('version')!r}, 요청={version}")
    repository = data.get("repository")
    if not isinstance(repository, dict):
        error(f"{relative}: repository 객체 누락")
    else:
        if repository.get("type") != "git":
            error(f"{relative}: repository.type은 git이어야 함")
        if repository.get("url") != "https://github.com/zlink-systems/zlink.git":
            error(f"{relative}: repository.url이 공식 저장소가 아님")
        if repository.get("directory") != expected_directory:
            error(f"{relative}: repository.directory={repository.get('directory')!r}")
    if data.get("homepage") != "https://zlink.systems/":
        error(f"{relative}: homepage가 https://zlink.systems/가 아님")


def validate_npm():
    if target == "bindings":
        npm_package("bindings/node/package.json", "bindings/node")
    elif target == "framework":
        for package in (
            "stream-wire",
            "stream-connector",
            "framework",
            "framework-codec-protobuf",
            "framework-codec-msgpack",
            "framework-locations-redis",
            "http-client",
            "nestjs",
        ):
            npm_package(
                f"framework/languages/node/packages/{package}/package.json",
                f"framework/languages/node/packages/{package}",
            )


def validate_nuget():
    if target == "bindings":
        project = "bindings/dotnet/src/Zlink/Zlink.csproj"
        contains(
            project,
            "ZLinkBindingVersionFile",
            "ZLINK_BINDING_VERSION=",
            "<PackageId>Zlink</PackageId>",
            "<Description>",
        )
        contains(
            "bindings/dotnet/Directory.Build.targets",
            "<Authors>zlink</Authors>",
            "<PackageLicenseExpression",
            "<PackageProjectUrl",
            "<RepositoryUrl",
            "<RepositoryType",
            "<PackageReadmeFile",
            "<PublishRepositoryUrl",
        )
        return

    props = "framework/languages/dotnet/Directory.Build.props"
    contains(
        props,
        "ZLinkFrameworkVersionFile",
        "ZLINK_FRAMEWORK_VERSION=",
        "<PackageLicenseFile>LICENSE</PackageLicenseFile>",
        "<PackageProjectUrl>https://github.com/zlink-systems/zlink</PackageProjectUrl>",
    )
    expected_names = {
        "Systems.Zlink.Stream.Connector",
        "Zlink.Framework.AspNetCore",
        "Zlink.Framework.Codecs.MessagePack",
        "Zlink.Framework.Codecs.Protobuf",
        "Zlink.Framework.Contracts",
        "Zlink.Framework.Locations.Redis",
        "Zlink.Framework.Provider.Abstractions",
        "Zlink.Framework",
        "Zlink.HttpClient",
    }
    projects = []
    source_root = root / "framework/languages/dotnet/src"
    if source_root.is_dir():
        for path in sorted(source_root.glob("*/*.csproj")):
            source = text(path.relative_to(root).as_posix())
            if "<IsPackable>true</IsPackable>" in source:
                projects.append((path, source))
    actual_names = {path.parent.name for path, _source in projects}
    if actual_names != expected_names:
        missing = sorted(expected_names - actual_names)
        extra = sorted(actual_names - expected_names)
        error(f"framework NuGet 배포 목록 불일치: 누락={missing}, 추가={extra}")
    for path, source in projects:
        if "<Description>" not in source:
            error(f"{path.relative_to(root)}: Description 누락")
        package_id = re.search(r"<PackageId>([^<]+)</PackageId>", source)
        effective_id = package_id.group(1) if package_id else path.stem
        if not (effective_id == "Zlink" or effective_id.startswith("Zlink.")):
            error(f"{path.relative_to(root)}: 공개 PackageId가 Zlink.*가 아님: {effective_id}")


def validate_maven():
    if target == "bindings":
        build = "bindings/java/build.gradle"
        contains(
            build,
            "group = 'systems.zlink'",
            f"version = '{version}'",
            "url = 'https://github.com/zlink-systems/zlink'",
            "licenses {",
            "developers {",
            "scm {",
            "withSourcesJar()",
            "withJavadocJar()",
            "centralStaging",
        )
        script = "bindings/java/scripts/upload-central-bundle.sh"
        contains(script, "ZLINK_BINDING_VERSION", '"$java_root/VERSION"', "zlink-java-$version-central-bundle.zip")
        contains(
            "bindings/java/codec/zlink-ext-netty/build.gradle",
            "maven-publish",
            "description",
        )
    else:
        build = "framework/languages/java/build.gradle.kts"
        contains(
            build,
            'group = "systems.zlink"',
            'layout.projectDirectory.file("VERSION")',
            'url.set("https://github.com/zlink-systems/zlink")',
            "licenses {",
            "developers {",
            "scm {",
            "withSourcesJar()",
            "withJavadocJar()",
            'name = "centralStaging"',
        )
        script = "framework/languages/java/scripts/upload-central-bundle.sh"
        contains(script, "ZLINK_FRAMEWORK_VERSION", '"$java_root/VERSION"', "zlink-framework-java-$version-central-bundle.zip")
        expected_names = {
            "zlink-framework-provider-abstractions",
            "zlink-framework-binding-internal",
            "zlink-framework-json-internal",
            "zlink-framework-core",
            "zlink-framework-codec-protobuf",
            "zlink-framework-codec-msgpack",
            "zlink-framework-locations-redis",
            "zlink-http-client",
            "zlink-framework-spring-boot-starter",
            "zlink-stream-connector",
            "zlink-framework-kotlin",
            "zlink-http-client-kotlin",
            "zlink-framework-testkit",
        }
        published = []
        java_root = root / "framework/languages/java"
        if java_root.is_dir():
            for path in sorted(java_root.glob("zlink-*/build.gradle.kts")):
                source = text(path.relative_to(root).as_posix())
                if "`maven-publish`" in source:
                    published.append(path)
                    if "description" not in source:
                        error(f"{path.relative_to(root)}: description 누락")
        actual_names = {path.parent.name for path in published}
        if actual_names != expected_names:
            missing = sorted(expected_names - actual_names)
            extra = sorted(actual_names - expected_names)
            error(f"framework Maven 배포 목록 불일치: 누락={missing}, 추가={extra}")


def conan_source(relative, tag, asset):
    source = text(relative)
    match = re.search(
        rf'(?ms)^  "{re.escape(version)}":\s*\n(?P<body>(?:    .*(?:\n|$))*)',
        source,
    )
    if not match:
        error(f"{relative}: {version} source 항목 누락")
        return
    body = match.group("body")
    url = re.search(r'^    url:\s*"([^"]+)"', body, re.MULTILINE)
    digest = re.search(r'^    sha256:\s*"([0-9a-f]{64})"\s*$', body, re.MULTILINE)
    if not url or tag not in url.group(1) or asset not in url.group(1):
        error(f"{relative}: {version} URL이 태그/자산과 일치하지 않음")
    if not digest or set(digest.group(1)) == {"0"}:
        error(f"{relative}: {version} sha256이 64자리 소문자 16진수가 아님")


def vcpkg_port(manifest, portfile, tag, asset=None):
    data = json_file(manifest)
    if data.get("version") != version:
        error(f"{manifest}: version={data.get('version')!r}, 요청={version}")
    source = text(portfile)
    if tag not in source and "${VERSION}" not in source:
        error(f"{portfile}: 릴리스 태그 버전 누락")
    if asset and asset not in source and asset.replace(version, "${VERSION}") not in source:
        error(f"{portfile}: 릴리스 자산 이름 누락")
    digest = re.search(r"(?m)^\s*SHA512\s+([0-9a-f]{128})\s*$", source)
    if not digest or set(digest.group(1)) == {"0"}:
        error(f"{portfile}: SHA512가 128자리 소문자 16진수가 아님")


def validate_cpp():
    if target == "core":
        vcpkg_port(
            "vcpkg/ports/zlink/vcpkg.json",
            "vcpkg/ports/zlink/portfile.cmake",
            f"core/v{version}",
        )
        conan_source(
            "core/packaging/conan/conandata.yml",
            f"core/v{version}",
            f"zlink-{version}-source.tar.gz",
        )
        return

    if target == "bindings":
        prefix = "bindings/cpp/packaging/conan"
        port = "zlink-cpp"
        tag = f"cpp/v{version}"
        asset = f"zlink-cpp-{version}.tar.gz"
    else:
        prefix = "framework/languages/cpp/packaging/conan"
        port = "zlink-framework"
        tag = f"framework-cpp/v{version}"
        asset = f"zlink-framework-cpp-{version}.tar.gz"
    vcpkg_port(
        f"vcpkg/ports/{port}/vcpkg.json",
        f"vcpkg/ports/{port}/portfile.cmake",
        tag,
        asset,
    )
    contains(f"{prefix}/conanfile.py", f'version = "{version}"')
    conan_source(f"{prefix}/conandata.yml", tag, asset)


validators = {
    "npm": validate_npm,
    "nuget": validate_nuget,
    "maven": validate_maven,
    "cpp": validate_cpp,
}
validators[mode]()
if errors:
    for item in errors:
        print(f"메타데이터 오류: {item}", file=sys.stderr)
    sys.exit(1)
PY
}

check_metadata_item() {
    local root=$1 mode=$2 display=$3 detail=$4
    if validate_metadata "$root" "$mode"; then
        record_check '패키지 메타데이터' "$display" PASS "$detail"
    else
        record_check '패키지 메타데이터' "$display" FAIL "$detail"
    fi
}

check_package_metadata() {
    local root=$1
    case "$RELEASE_TARGET" in
        core)
            check_metadata_item "$root" cpp 'Conan·vcpkg' 'Core recipe·port 버전과 SHA'
            ;;
        bindings)
            case "$RELEASE_LANGUAGE" in
                node) check_metadata_item "$root" npm npm '@zlink-systems/zlink repository·homepage' ;;
                dotnet) check_metadata_item "$root" nuget NuGet 'Zlink nupkg 메타데이터' ;;
                java) check_metadata_item "$root" maven Maven 'systems.zlink 2개·Central bundle' ;;
                cpp) check_metadata_item "$root" cpp 'Conan·vcpkg' 'zlink-cpp recipe·port 버전과 SHA' ;;
            esac
            ;;
        framework)
            case "$RELEASE_LANGUAGE" in
                node) check_metadata_item "$root" npm npm '@zlink-systems/* 8개 repository·homepage' ;;
                dotnet) check_metadata_item "$root" nuget NuGet 'Zlink.* 9개 nupkg 메타데이터' ;;
                java) check_metadata_item "$root" maven Maven 'systems.zlink 13개·Central bundle' ;;
                cpp) check_metadata_item "$root" cpp 'Conan·vcpkg' 'zlink-framework recipe·port 버전과 SHA' ;;
            esac
            ;;
    esac
}

print_check_table() {
    local index
    printf '\n검사 결과\n'
    printf '| 구분 | 배포 대상 | 결과 | 확인 내용 |\n'
    printf '|---|---|---|---|\n'
    for index in "${!CHECK_AREAS[@]}"; do
        printf '| %s | %s | %s | %s |\n' \
            "${CHECK_AREAS[$index]}" "${CHECK_TARGETS[$index]}" \
            "${CHECK_RESULTS[$index]}" "${CHECK_DETAILS[$index]}"
    done
}

print_deployment_targets() {
    printf '\n배포 대상 체크리스트\n'
    printf '| 순서 | 태그·대상 | 공개 채널 |\n'
    printf '|---:|---|---|\n'
    case "$RELEASE_TARGET" in
        core)
            printf '| 1 | core/v%s | GitHub Release (native 5종·source·checksum·provenance) |\n' "$RELEASE_VERSION"
            printf '| 2 | zlink recipe | ConanCenter PR |\n'
            printf '| 3 | zlink port | microsoft/vcpkg PR |\n'
            ;;
        bindings)
            case "$RELEASE_LANGUAGE" in
                cpp) printf '| 1 | cpp/v%s | GitHub Release + ConanCenter·vcpkg PR |\n' "$RELEASE_VERSION" ;;
                node) printf '| 1 | node/v%s | npm |\n' "$RELEASE_VERSION" ;;
                java) printf '| 1 | java/v%s | Maven Central |\n' "$RELEASE_VERSION" ;;
                dotnet) printf '| 1 | dotnet/v%s | nuget.org |\n' "$RELEASE_VERSION" ;;
            esac
            ;;
        framework)
            case "$RELEASE_LANGUAGE" in
                cpp) printf '| 1 | framework-cpp/v%s | GitHub Release + ConanCenter·vcpkg PR |\n' "$RELEASE_VERSION" ;;
                node) printf '| 1 | framework-node/v%s | npm (8개) |\n' "$RELEASE_VERSION" ;;
                java) printf '| 1 | framework-java/v%s | Maven Central (13개) |\n' "$RELEASE_VERSION" ;;
                dotnet) printf '| 1 | framework-dotnet/v%s | nuget.org (9개) |\n' "$RELEASE_VERSION" ;;
            esac
            ;;
    esac
}

main() {
    local root positional=() original=("$@")
    RESUME_COMMAND=$(quote_command "${BASH_SOURCE[0]}" "${original[@]}")
    while (($#)); do
        case "$1" in
            --dry-run) DRY_RUN=1; shift ;;
            -h|--help) usage; return 0 ;;
            --*) usage >&2; die 2 "알 수 없는 옵션입니다: $1" ;;
            *) positional+=("$1"); shift ;;
        esac
    done
    ((${#positional[@]} >= 2 && ${#positional[@]} <= 3)) \
        || { usage >&2; die 2 '대상, 선택 언어와 버전을 지정해야 합니다.'; }
    RELEASE_TARGET=${positional[0]}
    case "$RELEASE_TARGET" in
        core)
            ((${#positional[@]} == 2)) \
                || { usage >&2; die 2 'Core는 core <X.Y.Z> 형식으로 지정합니다.'; }
            RELEASE_VERSION=${positional[1]}
            ;;
        bindings|framework)
            ((${#positional[@]} == 3)) \
                || { usage >&2; die 2 "$RELEASE_TARGET 릴리스에는 언어가 필요합니다."; }
            RELEASE_LANGUAGE=${positional[1]}
            case "$RELEASE_LANGUAGE" in
                cpp|node|java|dotnet) ;;
                *) usage >&2; die 2 "지원하지 않는 $RELEASE_TARGET 언어입니다: $RELEASE_LANGUAGE" ;;
            esac
            RELEASE_VERSION=${positional[2]}
            ;;
        *) usage >&2; die 2 "지원하지 않는 릴리스 대상입니다: $RELEASE_TARGET" ;;
    esac
    [[ "$RELEASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
        || die 2 "버전은 X.Y.Z 형식이어야 합니다: $RELEASE_VERSION"

    if [[ "$RELEASE_TARGET" == core ]]; then
        RESUME_COMMAND=$(quote_command "${BASH_SOURCE[0]}" "$RELEASE_TARGET" "$RELEASE_VERSION")
    else
        RESUME_COMMAND=$(quote_command "${BASH_SOURCE[0]}" "$RELEASE_TARGET" "$RELEASE_LANGUAGE" "$RELEASE_VERSION")
    fi
    require_command python3
    root=$(repo_root)
    if ((DRY_RUN)); then
        printf 'dry-run: 읽기 전용 검사를 동일하게 수행하며 파일이나 외부 상태를 바꾸지 않습니다.\n'
    fi
    printf '릴리스 사전 검사: %s%s %s\n' "$RELEASE_TARGET" "${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" "$RELEASE_VERSION"

    check_version_sync "$root"
    check_release_notes "$root"
    check_package_metadata "$root"
    print_check_table
    print_deployment_targets

    if ((FAILURES > 0)); then
        die 1 "$FAILURES개 검사가 실패했습니다. 수정한 뒤 같은 명령으로 재개하십시오."
    fi
    printf '\n릴리스 사전 검사 통과: %s%s %s\n' "$RELEASE_TARGET" "${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" "$RELEASE_VERSION"
}

main "$@"
