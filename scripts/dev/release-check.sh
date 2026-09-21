#!/usr/bin/env bash

set -euo pipefail

DRY_RUN=0
WRITE_MODE=0
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
  release-check.sh --write [--dry-run] core <X.Y.Z>
  release-check.sh --write [--dry-run] bindings cpp <X.Y.Z>
  release-check.sh --write [--dry-run] framework cpp <X.Y.Z>

--write는 vcpkg port·Conan recipe(vcpkg.json·portfile.cmake·conandata.yml·
conanfile.py)에 릴리스 태그·자산의 버전·SHA를 채운다. core 대상과
bindings/framework의 cpp 언어에서만 의미가 있다(다른 언어는 npm·NuGet·Maven이라
이 자동화의 대상이 아니다). --write와 함께 쓴 --dry-run은 값을 계산해 보여주기만
하고 파일을 바꾸지 않는다.
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

    if bash "$root/scripts/dev/version-literals.sh" >/dev/null 2>&1; then
        record_check 버전 scripts PASS 'version-literals.sh: scripts/에 버전 리터럴 없음'
    else
        record_check 버전 scripts FAIL 'version-literals.sh: scripts/에 버전 리터럴 (VERSION 파일을 읽어야 함)'
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
    local root=$1 mode=$2 action=${3:-check} dry_run=${4:-0}
    python3 - "$root" "$RELEASE_TARGET" "$RELEASE_LANGUAGE" "$RELEASE_VERSION" "$mode" "$action" "$dry_run" <<'PY'
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

root = Path(sys.argv[1])
target = sys.argv[2]
language = sys.argv[3]
version = sys.argv[4]
mode = sys.argv[5]
action = sys.argv[6] if len(sys.argv) > 6 else "check"
dry_run = (sys.argv[7] if len(sys.argv) > 7 else "0") == "1"
errors = []
GITHUB_REPO = "zlink-systems/zlink"
# Overridable only so release-check-test.sh can point write mode at a local
# fixture server instead of the real GitHub; unset in every real invocation.
GITHUB_BASE_URL = os.environ.get(
    "ZLINK_RELEASE_CHECK_GITHUB_BASE_URL", "https://github.com"
)


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


# The one target table check and write both read. A vcpkg port·Conan recipe
# pair exists for exactly three release targets; keeping their paths and
# tag/asset naming in a single place is the point of this module (issue #378):
# check and write must never be able to disagree about what "correct" means.
CPP_TARGETS = {
    ("core", None): {
        "vcpkg_manifest": "vcpkg/ports/zlink/vcpkg.json",
        "portfile": "vcpkg/ports/zlink/portfile.cmake",
        "conandata": "core/packaging/conan/conandata.yml",
        "conanfile": None,
        "tag_template": "core/v{version}",
        "asset_template": "zlink-{version}-source.tar.gz",
    },
    ("bindings", "cpp"): {
        "vcpkg_manifest": "vcpkg/ports/zlink-cpp/vcpkg.json",
        "portfile": "vcpkg/ports/zlink-cpp/portfile.cmake",
        "conandata": "bindings/cpp/packaging/conan/conandata.yml",
        "conanfile": "bindings/cpp/packaging/conan/conanfile.py",
        "tag_template": "cpp/v{version}",
        "asset_template": "zlink-cpp-{version}.tar.gz",
    },
    ("framework", "cpp"): {
        "vcpkg_manifest": "vcpkg/ports/zlink-framework/vcpkg.json",
        "portfile": "vcpkg/ports/zlink-framework/portfile.cmake",
        "conandata": "framework/languages/cpp/packaging/conan/conandata.yml",
        "conanfile": "framework/languages/cpp/packaging/conan/conanfile.py",
        "tag_template": "framework-cpp/v{version}",
        "asset_template": "zlink-framework-cpp-{version}.tar.gz",
    },
}


def cpp_target():
    key = (target, None if target == "core" else language)
    row = CPP_TARGETS.get(key)
    if row is None:
        error(f"지원하지 않는 cpp 대상입니다: {target}/{language}")
        return None
    return {
        **row,
        "tag": row["tag_template"].format(version=version),
        "asset": row["asset_template"].format(version=version),
    }


def conan_source(relative, tag, asset, section="sources"):
    source = text(relative)
    # sources: and binaries: key their versions identically, so a whole-file
    # search for the version key finds whichever section comes first. Scope
    # the lookup the way write_conan_version_entry() already does.
    bounds = conan_section_range(source, section)
    if bounds is None:
        error(f"{relative}: {section}: 헤더를 찾지 못함")
        return
    match = re.search(
        rf'(?ms)^  "{re.escape(version)}":\s*\n(?P<body>(?:    .*(?:\n|$))*)',
        source[bounds[0]:bounds[1]],
    )
    if not match:
        error(f"{relative}: {section}: {version} 항목 누락")
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
    t = cpp_target()
    if t is None:
        return
    if target == "core":
        vcpkg_port(t["vcpkg_manifest"], t["portfile"], t["tag"])
        conan_source(t["conandata"], t["tag"], t["asset"])
        return
    vcpkg_port(t["vcpkg_manifest"], t["portfile"], t["tag"], t["asset"])
    contains(t["conanfile"], f'version = "{version}"')
    conan_source(t["conandata"], t["tag"], t["asset"])


# --- write mode -------------------------------------------------------
#
# Fills the same fields validate_cpp() checks, from the published release
# tag and asset. Values depend on state that only exists after a release is
# published (the tag and its GitHub Release asset), so this network access
# is unavoidable; it happens only in write mode, never in check mode.


def download(url):
    request = urllib.request.Request(url, headers={"User-Agent": "zlink-release-check"})
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            return response.read()
    except (OSError, urllib.error.URLError) as exc:
        error(f"{url}: 다운로드 실패: {exc}")
        return None


def detect_archive_kind(portfile_source, portfile):
    # A port's fallback-to-source-build (vcpkg_from_github) and its per-platform
    # prebuilt archives (vcpkg_download_distfile with one SHA512 per platform
    # behind an if/elseif chain, chosen by triplet -- PR #413's shape for this
    # port) legitimately coexist: each owns an unambiguous hash of its own, so
    # their mere co-occurrence is not what makes a port unreadable. What is
    # still unreadable is a vcpkg_from_github next to a vcpkg_download_distfile
    # whose SHA512 is itself a literal -- both would then be asking for the
    # hash of "the one archive" and nothing here says which literal belongs to
    # which, so that combination still refuses to guess.
    has_github = bool(re.search(r"(?m)^\s*vcpkg_from_github\(", portfile_source))
    distfile_call = re.search(r"vcpkg_download_distfile\([^)]*\)", portfile_source, re.DOTALL)
    distfile_shape = None
    if distfile_call:
        call_text = distfile_call.group(0)
        if re.search(r"SHA512\s+[0-9a-fA-F]+\b", call_text):
            distfile_shape = "literal"
        elif re.search(r'SHA512\s+"?\$\{\w+\}"?', call_text):
            distfile_shape = "platform"
        else:
            error(f"{portfile}: vcpkg_download_distfile()의 SHA512 형태를 알아볼 수 없음")
            return None
    if has_github and distfile_shape == "literal":
        error(
            f"{portfile}: vcpkg_from_github/vcpkg_download_distfile 형태를 판정할 수 "
            "없음(SHA512가 무엇의 해시인지 정할 수 없어 계산을 거부함)"
        )
        return None
    if not has_github and distfile_shape is None:
        error(f"{portfile}: vcpkg_from_github도 vcpkg_download_distfile도 찾지 못함")
        return None
    return {"github": has_github, "distfile": distfile_shape}


def write_text_if_changed(relative, new_source):
    path = root / relative
    try:
        old_source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        error(f"{relative}: 읽기 실패: {exc}")
        return
    if old_source == new_source:
        print(f"변경 없음: {relative}")
        return
    if dry_run:
        print(f"dry-run: 갱신 예정: {relative}")
        return
    path.write_text(new_source, encoding="utf-8")
    print(f"갱신함: {relative}")


def regex_replace_file(relative, pattern, replacement, what):
    source = text(relative)
    if not source:
        return
    new_source, count = pattern.subn(replacement, source, count=1)
    if count == 0:
        error(f"{relative}: {what} 필드를 찾지 못함")
        return
    write_text_if_changed(relative, new_source)


def template_regex(template):
    # Exactly one "{version}" placeholder; matches that slot with any X.Y.Z so
    # a template ("...${VERSION}...") is left untouched (nothing to match)
    # while a stale literal version is replaced.
    prefix, _, suffix = template.partition("{version}")
    return re.compile(re.escape(prefix) + r"[0-9]+\.[0-9]+\.[0-9]+" + re.escape(suffix))


def replace_call_sha512(source, call_name, new_hash, portfile):
    """Replace the literal SHA512 hex value inside this file's one
    call_name(...) call, without touching any other SHA512-looking text
    elsewhere in the file (there may be several, e.g. one per platform)."""
    match = re.search(rf"{call_name}\([^)]*\)", source, re.DOTALL)
    if match is None:
        error(f"{portfile}: {call_name}() 호출을 찾지 못함")
        return None
    call_text, count = re.subn(
        r"(?m)^(\s*SHA512\s+)[0-9a-fA-F]+(\s*)$",
        rf"\g<1>{new_hash}\g<2>",
        match.group(0),
        count=1,
    )
    if count == 0:
        error(f"{portfile}: {call_name}()의 SHA512 필드를 찾지 못함")
        return None
    return source[: match.start()] + call_text + source[match.end() :]


def portfile_platform_archives(portfile_source, tag, portfile):
    """What vcpkg_download_distfile()'s if/elseif chain currently downloads:
    one archive per platform, named by substituting that platform into the
    same URL template vcpkg itself will use. Returns an ordered
    {platform: url} map (file order, i.e. the if/elseif chain's order), read
    from the chain itself rather than a fixed platform list -- another port
    with this shape, or a chain that grows a fifth platform, needs no change
    here. Returns None on error (nothing this specific to the file's own
    shape could be found)."""
    distfile_call = re.search(r"vcpkg_download_distfile\([^)]*\)", portfile_source, re.DOTALL)
    call_text = distfile_call.group(0)
    sha_var_match = re.search(r'SHA512\s+"?\$\{(\w+)\}"?', call_text)
    url_match = re.search(r'URLS\s+"([^"]+)"', call_text)
    if sha_var_match is None or url_match is None:
        error(f"{portfile}: vcpkg_download_distfile()에서 SHA512/URLS를 읽지 못함")
        return None
    sha_var = sha_var_match.group(1)
    filename_template = url_match.group(1).rsplit("/", 1)[-1]

    pairs = re.findall(
        rf'set\((\w+)\s+"([a-z0-9][a-z0-9-]*)"\)\s*\n\s*'
        rf'set\({re.escape(sha_var)}\s+"[0-9a-fA-F]+"\)',
        portfile_source,
    )
    if not pairs:
        error(f"{portfile}: {sha_var}과 짝을 이루는 플랫폼 set() 쌍을 찾지 못함")
        return None

    archives = {}
    for platform_var, platform in pairs:
        filename = filename_template.replace("${" + platform_var + "}", platform)
        archives[platform] = (
            platform_var,
            sha_var,
            f"{GITHUB_BASE_URL}/{GITHUB_REPO}/releases/download/{tag}/{filename}",
        )
    return archives


def write_platform_archive_hash(source, platform_var, platform, sha_var, new_hash, portfile):
    pattern = re.compile(
        rf'(set\({re.escape(platform_var)}\s+"{re.escape(platform)}"\)\s*\n\s*'
        rf'set\({re.escape(sha_var)}\s+")[0-9a-fA-F]+(")'
    )
    new_source, count = pattern.subn(rf"\g<1>{new_hash}\g<2>", source, count=1)
    if count == 0:
        error(f"{portfile}: {platform} 플랫폼의 SHA512 set()을 갱신하지 못함")
        return None
    return new_source


def conan_section_range(conandata_source, section):
    """Byte range of the top-level `section:` block: its header line through
    the last line indented (or blank) under it. None if the header is absent."""
    header = re.search(rf"(?m)^{re.escape(section)}:\s*\n", conandata_source)
    if header is None:
        return None
    body = re.compile(r"(?:(?!^\S).*\n?)*", re.MULTILINE)
    body_match = body.match(conandata_source, header.end())
    return header.start(), body_match.end()


def write_conan_version_entry(conandata, section, version, body_lines):
    """Add or replace this version's entry under the top-level `section:` key,
    scoped strictly to that section -- `sources:` and `binaries:` both key
    their versions the same way, and a naive whole-file search for
    '  "{version}":' would find whichever section happens to come first."""
    conandata_source = text(conandata)
    if not conandata_source:
        return
    bounds = conan_section_range(conandata_source, section)
    if bounds is None:
        error(f"{conandata}: {section}: 헤더를 찾지 못함")
        return
    start, end = bounds
    section_text = conandata_source[start:end]
    new_entry = f'  "{version}":\n' + "".join(body_lines)
    # A blank line ends the entry too, not just the next version key or a
    # column-0 line -- otherwise the last entry in a section swallows the
    # blank line that separates this section from the next one, and
    # regenerating that entry's body loses it.
    entry_re = re.compile(rf'(?m)^  "{re.escape(version)}":\n(?:(?!^  "|^\s*$).*\n?)*')
    if entry_re.search(section_text):
        new_section_text = entry_re.sub(new_entry, section_text, count=1)
    else:
        new_section_text, header_count = re.subn(
            rf"(?m)^{re.escape(section)}:\s*\n", f"{section}:\n" + new_entry, section_text, count=1
        )
        if header_count == 0:
            error(f"{conandata}: {section}: 헤더를 찾지 못함")
            return
    write_text_if_changed(conandata, conandata_source[:start] + new_section_text + conandata_source[end:])


def write_cpp():
    t = cpp_target()
    if t is None:
        return

    portfile_source = text(t["portfile"])
    if not portfile_source:
        return
    kind = detect_archive_kind(portfile_source, t["portfile"])
    if kind is None:
        return

    # The generic release-source archive backs conandata.yml's `sources:`
    # entry in every case -- it is what a static triplet (or any platform
    # with no prebuilt archive) still builds Core from, whether or not this
    # port also has platform archives.
    asset_url = f"{GITHUB_BASE_URL}/{GITHUB_REPO}/releases/download/{t['tag']}/{t['asset']}"
    asset_bytes = download(asset_url)
    if asset_bytes is None:
        return
    asset_sha256 = hashlib.sha256(asset_bytes).hexdigest()

    github_sha512 = None
    if kind["github"]:
        source_url = f"{GITHUB_BASE_URL}/{GITHUB_REPO}/archive/{t['tag']}.tar.gz"
        source_bytes = download(source_url)
        if source_bytes is None:
            return
        github_sha512 = hashlib.sha512(source_bytes).hexdigest()

    distfile_sha512 = None
    if kind["distfile"] == "literal":
        distfile_sha512 = hashlib.sha512(asset_bytes).hexdigest()

    platform_archives = None
    platform_hashes = None
    if kind["distfile"] == "platform":
        platform_archives = portfile_platform_archives(portfile_source, t["tag"], t["portfile"])
        if platform_archives is None:
            return
        platform_hashes = {}
        for platform, (platform_var, sha_var, url) in platform_archives.items():
            archive_bytes = download(url)
            if archive_bytes is None:
                return
            platform_hashes[platform] = (
                platform_var,
                sha_var,
                url,
                hashlib.sha512(archive_bytes).hexdigest(),
                hashlib.sha256(archive_bytes).hexdigest(),
            )

    # Every byte needed is downloaded and hashed above; nothing below fails
    # partway through writing a file with only some of a release's values.

    # vcpkg.json: version
    regex_replace_file(
        t["vcpkg_manifest"],
        re.compile(r'"version":\s*"[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?"'),
        f'"version": "{version}"',
        "version",
    )

    # portfile.cmake: tag, asset and release-version literals (each a no-op
    # where already ${VERSION}-templated), then whichever SHA512 field(s)
    # this port's shape has.
    new_portfile = template_regex(t["tag_template"]).sub(t["tag"], portfile_source)
    new_portfile = template_regex(t["asset_template"]).sub(t["asset"], new_portfile)
    new_portfile = re.sub(
        r'(set\(ZLINK_RELEASE_VERSION\s+")[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?("\))',
        rf"\g<1>{version}\g<2>",
        new_portfile,
    )
    if github_sha512 is not None:
        new_portfile = replace_call_sha512(new_portfile, "vcpkg_from_github", github_sha512, t["portfile"])
        if new_portfile is None:
            return
    if distfile_sha512 is not None:
        new_portfile = replace_call_sha512(
            new_portfile, "vcpkg_download_distfile", distfile_sha512, t["portfile"]
        )
        if new_portfile is None:
            return
    if platform_hashes is not None:
        for platform, (platform_var, sha_var, _url, sha512, _sha256) in platform_hashes.items():
            new_portfile = write_platform_archive_hash(
                new_portfile, platform_var, platform, sha_var, sha512, t["portfile"]
            )
            if new_portfile is None:
                return
    write_text_if_changed(t["portfile"], new_portfile)

    # conandata.yml: the generic source entry always, plus one binaries:
    # entry per platform when this port has platform archives.
    write_conan_version_entry(
        t["conandata"], "sources", version,
        [f'    url: "{asset_url}"\n', f'    sha256: "{asset_sha256}"\n'],
    )
    if platform_hashes is not None:
        body_lines = []
        for platform, (_platform_var, _sha_var, url, _sha512, sha256) in platform_hashes.items():
            body_lines.append(f"    {platform}:\n")
            body_lines.append(f'      url: "{url}"\n')
            body_lines.append(f'      sha256: "{sha256}"\n')
        write_conan_version_entry(t["conandata"], "binaries", version, body_lines)

    # conanfile.py: version (bindings/framework only; core has no conanfile.py)
    if t["conanfile"]:
        regex_replace_file(
            t["conanfile"],
            re.compile(r'(?m)^(\s*version\s*=\s*)"[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?"'),
            rf'\g<1>"{version}"',
            "version",
        )


validators = {
    "npm": validate_npm,
    "nuget": validate_nuget,
    "maven": validate_maven,
    "cpp": validate_cpp,
}
if action == "write":
    if mode != "cpp":
        error(f"쓰기 모드는 cpp에서만 지원합니다: mode={mode}")
    else:
        write_cpp()
else:
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

cpp_write_supported() {
    case "$RELEASE_TARGET" in
        core) return 0 ;;
        bindings|framework) [[ "$RELEASE_LANGUAGE" == cpp ]] ;;
        *) return 1 ;;
    esac
}

cmd_write() {
    local root=$1
    cpp_write_supported \
        || die 2 '쓰기 모드는 core 또는 bindings/framework의 cpp 대상에만 있습니다(vcpkg·Conan 레시피).'
    printf '릴리스 레시피 쓰기: %s%s %s\n' "$RELEASE_TARGET" "${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" "$RELEASE_VERSION"
    ((DRY_RUN)) && printf 'dry-run: 값을 계산해 보여주기만 하고 파일을 바꾸지 않습니다.\n'
    if validate_metadata "$root" cpp write "$DRY_RUN"; then
        printf '\n쓰기 완료: %s%s %s\n' "$RELEASE_TARGET" "${RELEASE_LANGUAGE:+/$RELEASE_LANGUAGE}" "$RELEASE_VERSION"
    else
        die 1 '릴리스 레시피 쓰기가 실패했습니다.'
    fi
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
            --write) WRITE_MODE=1; shift ;;
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

    if ((WRITE_MODE)); then
        cmd_write "$root"
        return 0
    fi

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
