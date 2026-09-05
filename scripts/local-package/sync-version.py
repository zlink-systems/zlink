#!/usr/bin/env python3
"""Synchronize Core and first-party binding package versions."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path
from typing import Callable


REPO_ROOT = Path(__file__).resolve().parents[2]
SEMVER = r"[0-9]+\.[0-9]+\.[0-9]+"


class SyncError(RuntimeError):
    pass


def repository_version() -> tuple[str, str, str, str]:
    values: dict[str, str] = {}
    for line in (REPO_ROOT / "VERSION").read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            raise SyncError(f"VERSION contains an invalid line: {line!r}")
        key, value = line.split("=", 1)
        if key in values:
            raise SyncError(f"VERSION contains duplicate field {key}")
        values[key] = value
    expected = {
        "LIBZLINK_VERSION_MAJOR",
        "LIBZLINK_VERSION_MINOR",
        "LIBZLINK_VERSION_PATCH",
        "LIBZLINK_VERSION",
    }
    if set(values) != expected:
        raise SyncError("VERSION must contain exactly the four LIBZLINK_VERSION fields")
    major = values["LIBZLINK_VERSION_MAJOR"]
    minor = values["LIBZLINK_VERSION_MINOR"]
    patch = values["LIBZLINK_VERSION_PATCH"]
    if not all(re.fullmatch(r"0|[1-9][0-9]*", part) for part in (major, minor, patch)):
        raise SyncError("VERSION components must be canonical non-negative integers")
    version = values["LIBZLINK_VERSION"]
    if version != f"{major}.{minor}.{patch}":
        raise SyncError("LIBZLINK_VERSION does not match its major/minor/patch fields")
    return major, minor, patch, version


def bindings_version() -> str:
    """Read the independently released first-party binding package version."""
    path = REPO_ROOT / "BINDINGS_VERSION"
    lines = path.read_text(encoding="utf-8").splitlines()
    if len(lines) != 1 or not lines[0].startswith("ZLINK_BINDINGS_VERSION="):
        raise SyncError("BINDINGS_VERSION must contain exactly ZLINK_BINDINGS_VERSION=X.Y.Z")
    key, version = lines[0].split("=", 1)
    if key != "ZLINK_BINDINGS_VERSION" or not re.fullmatch(SEMVER, version):
        raise SyncError("BINDINGS_VERSION must contain ZLINK_BINDINGS_VERSION=X.Y.Z")
    return version


def file_sha256(relative: str) -> str:
    return hashlib.sha256((REPO_ROOT / relative).read_bytes()).hexdigest()


class Synchronizer:
    def __init__(self, write: bool) -> None:
        self.write = write
        self.changed: list[Path] = []

    def transform(self, relative: str, transform: Callable[[str], str]) -> None:
        path = REPO_ROOT / relative
        source = path.read_text(encoding="utf-8")
        result = transform(source)
        if result == source:
            return
        if path not in self.changed:
            self.changed.append(path)
        if self.write:
            path.write_text(result, encoding="utf-8")

    def regex(
        self,
        relative: str,
        pattern: str,
        replacement: str | Callable[[re.Match[str]], str],
        expected: int,
        flags: int = 0,
    ) -> None:
        def apply(source: str) -> str:
            result, count = re.subn(pattern, replacement, source, flags=flags)
            if count != expected:
                raise SyncError(
                    f"{relative}: expected {expected} managed version field(s), found {count}"
                )
            return result

        self.transform(relative, apply)


def update_framework_node_dependency(source: str, version: str, expected: int) -> str:
    pattern = re.compile(r'("@zlink-systems/zlink"\s*:\s*")([^"]+)(")')

    def replacement(match: re.Match[str]) -> str:
        value = match.group(2)
        if value.startswith("file:"):
            value = re.sub(
                rf"zlink-systems-zlink-{SEMVER}\.tgz$",
                f"zlink-systems-zlink-{version}.tgz",
                value,
            )
        else:
            value = version
        return f"{match.group(1)}{value}{match.group(3)}"

    result, count = pattern.subn(replacement, source)
    if count != expected:
        raise SyncError(
            f"Framework Node manifest expected {expected} binding dependency pin(s), found {count}"
        )
    return result


def update_framework_node_lock(source: str, version: str) -> str:
    source = update_framework_node_dependency(source, version, 3)
    marker = '    "node_modules/@zlink-systems/zlink": {'
    start = source.find(marker)
    if start < 0:
        raise SyncError("Framework Node lockfile has no installed binding entry")
    end = source.find('\n    },\n    "node_modules/', start)
    if end < 0:
        raise SyncError("Framework Node lockfile binding entry is not structurally bounded")
    end += len("\n    },")
    block = source[start:end]
    block, version_count = re.subn(
        rf'("version"\s*:\s*"){SEMVER}(")',
        rf"\g<1>{version}\2",
        block,
        count=1,
    )
    block, resolved_count = re.subn(
        rf'("resolved"\s*:\s*"file:[^"]*zlink-systems-zlink-){SEMVER}(\.tgz")',
        rf"\g<1>{version}\2",
        block,
        count=1,
    )
    if version_count != 1 or resolved_count != 1:
        raise SyncError("Framework Node lockfile binding entry lacks version/resolved fields")
    block = re.sub(r'^\s*"integrity"\s*:\s*"[^"]+",\n', "", block, flags=re.MULTILINE)
    return source[:start] + block + source[end:]


def synchronize(write: bool) -> tuple[str, str, list[Path]]:
    major, minor, patch, core_version = repository_version()
    binding_version = bindings_version()
    version_path = f"{major}_{minor}_{patch}"
    sync = Synchronizer(write)

    sync.regex(
        "core/CMakeLists.txt",
        rf"project\(zlink VERSION {SEMVER} LANGUAGES C CXX\)",
        f"project(zlink VERSION {core_version} LANGUAGES C CXX)",
        1,
    )
    sync.regex(
        "core/packaging/debian/changelog",
        rf"(?m)^zlink \({SEMVER}-0\.1\)",
        f"zlink ({core_version}-0.1)",
        1,
    )
    sync.regex(
        "core/packaging/debian/changelog",
        rf"Package the zlink {SEMVER} public ABI\.",
        f"Package the zlink {core_version} public ABI.",
        1,
    )
    sync.regex(
        "core/packaging/debian/zlink.dsc",
        rf"(?m)^Version: {SEMVER}-0\.1$",
        f"Version: {core_version}-0.1",
        1,
    )
    sync.regex(
        "core/packaging/redhat/zlink.spec",
        rf"(?m)^Version:\s+{SEMVER}$",
        f"Version:       {core_version}",
        1,
    )
    sync.regex(
        "core/packaging/nuget/package.config",
        rf'(version\s*=\s*"){SEMVER}(")',
        rf"\g<1>{core_version}\2",
        1,
    )
    sync.regex(
        "core/packaging/nuget/package.config",
        r'(pathversion=")[0-9]+_[0-9]+_[0-9]+(")',
        rf"\g<1>{version_path}\2",
        1,
    )
    sync.regex(
        "core/packaging/nuget/package.nuspec",
        rf"(<version>){SEMVER}(</version>)",
        rf"\g<1>{core_version}\2",
        1,
    )
    sync.regex(
        "core/packaging/nuget/package.nuspec",
        r"[0-9]+_[0-9]+_[0-9]+",
        version_path,
        24,
    )
    sync.regex(
        "core/packaging/nuget/package.targets",
        r"[0-9]+_[0-9]+_[0-9]+",
        version_path,
        20,
    )
    for relative in (
        "core/include/zlink.h",
        "core/include/zlink/common.h",
        "bindings/c/include/zlink.h",
        "bindings/c/include/zlink/common.h",
        "bindings/cpp/include/zlink.h",
        "bindings/cpp/include/zlink/common.h",
        "bindings/go/include/zlink.h",
        "bindings/go/include/zlink/common.h",
        "bindings/rust/include/zlink.h",
        "bindings/rust/include/zlink/common.h",
    ):
        for name, value in (("MAJOR", major), ("MINOR", minor), ("PATCH", patch)):
            sync.regex(
                relative,
                rf"(?m)^#define ZLINK_VERSION_{name} [0-9]+$",
                f"#define ZLINK_VERSION_{name} {value}",
                1,
            )

    sync.regex(
        "bindings/cpp/CMakeLists.txt",
        rf"project\(zlink_cpp VERSION {SEMVER} LANGUAGES CXX\)",
        f"project(zlink_cpp VERSION {binding_version} LANGUAGES CXX)",
        1,
    )
    sync.regex(
        "bindings/cpp/CMakeLists.txt",
        rf'(set\(ZLINK_CPP_CORE_VERSION "){SEMVER}(" CACHE STRING)',
        rf"\g<1>{core_version}\2",
        1,
    )
    sync.regex(
        "bindings/cpp/CMakeLists.txt",
        rf"Path to an installed Zlink {SEMVER} Core release prefix",
        f"Path to an installed Zlink {core_version} Core release prefix",
        1,
    )
    sync.regex(
        "bindings/cpp/CMakeLists.txt",
        rf"Expected an installed Core {SEMVER} package",
        f"Expected an installed Core {core_version} package",
        1,
    )
    cpp_version_test = "bindings/cpp/tests/contract/test_cpp_contract_common_header_version.cpp"
    for name, value in (("MAJOR", major), ("MINOR", minor), ("PATCH", patch)):
        sync.regex(
            cpp_version_test,
            rf"(ZLINK_VERSION_{name} == )[0-9]+",
            rf"\g<1>{value}",
            1,
        )
    sync.regex(
        cpp_version_test,
        r"ZLINK_MAKE_VERSION\([0-9]+, [0-9]+, [0-9]+\)",
        f"ZLINK_MAKE_VERSION({major}, {minor}, {patch})",
        1,
    )
    for c_version_test in (
        "bindings/c/tests/test_c_common_header_version.c",
        "bindings/c/tests/test_c_contract_surface.c",
    ):
        for name, value in (("MAJOR", major), ("MINOR", minor), ("PATCH", patch)):
            sync.regex(
                c_version_test,
                rf"(ZLINK_VERSION_{name} == )[0-9]+",
                rf"\g<1>{value}",
                1,
            )
        sync.regex(
            c_version_test,
            r"ZLINK_MAKE_VERSION \([0-9]+, [0-9]+, [0-9]+\)",
            f"ZLINK_MAKE_VERSION ({major}, {minor}, {patch})",
            1,
        )
    sync.regex(
        "bindings/dotnet/src/Zlink/Zlink.csproj",
        rf"(<Version>){SEMVER}(</Version>)",
        rf"\g<1>{binding_version}\2",
        1,
    )
    sync.regex(
        "bindings/dotnet/src/Zlink/Zlink.csproj",
        rf"Core {SEMVER} package",
        f"Core {core_version} package",
        7,
    )
    sync.regex(
        "bindings/dotnet/src/Zlink/Runtime/Native/NativeLibraryLoader.cs",
        rf"(?<![0-9.]){SEMVER}(?![0-9.])",
        core_version,
        2,
    )

    java = "bindings/java/build.gradle"
    java_rules = (
        (rf"(?m)^version = '{SEMVER}'$", f"version = '{binding_version}'"),
        (rf"(systems\.zlink\.core\.version', String\),\s*)'{SEMVER}'", rf"\1'{core_version}'"),
        (rf"Core {SEMVER} install prefix", f"Core {core_version} install prefix"),
        (rf"metadata\.version != '{SEMVER}'", f"metadata.version != '{core_version}'"),
        (rf"Core package must report version {SEMVER}", f"Core package must report version {core_version}"),
        (rf"approved\.version != '{SEMVER}'", f"approved.version != '{core_version}'"),
        (rf"'zlink\.core\.version': '{SEMVER}'", f"'zlink.core.version': '{core_version}'"),
    )
    for pattern, replacement in java_rules:
        sync.regex(java, pattern, replacement, 1)

    sync.regex(
        "bindings/node/package.json",
        rf'("name"\s*:\s*"@zlink-systems/zlink",\s*\n\s*"version"\s*:\s*"){SEMVER}(")',
        rf"\g<1>{binding_version}\2",
        1,
    )
    sync.regex(
        "bindings/node/package-lock.json",
        rf'("name"\s*:\s*"@zlink-systems/zlink",\s*\n\s*"version"\s*:\s*"){SEMVER}(")',
        rf"\g<1>{binding_version}\2",
        2,
    )
    for relative in ("bindings/node/scripts/resolve_core.js", "bindings/node/scripts/verify_prebuilds.js"):
        expected = 2 if relative.endswith("resolve_core.js") else 1
        sync.regex(relative, rf"(?<![0-9.]){SEMVER}(?![0-9.])", core_version, expected)

    sync.regex(
        "bindings/go/internal/native/raw_contract_test.go",
        rf'(allowlist\.CoreVersion != "){SEMVER}("\s*)',
        rf"\g<1>{core_version}\2",
        1,
    )
    sync.regex(
        "bindings/go/tests/raw-core11-allowlist.json",
        rf'("coreVersion"\s*:\s*"){SEMVER}("\s*,)',
        rf"\g<1>{core_version}\2",
        1,
    )
    for header_path, source_path in (
        ("include/zlink.h", "core/include/zlink.h"),
        ("include/zlink/common.h", "core/include/zlink/common.h"),
    ):
        sync.regex(
            "bindings/go/tests/raw-core11-allowlist.json",
            rf'("path"\s*:\s*"{re.escape(header_path)}"\s*,\s*\n\s*"sha256"\s*:\s*")[0-9a-f]{{64}}("\s*)',
            rf"\g<1>{file_sha256(source_path)}\2",
            1,
        )
    sync.regex(
        "bindings/python/pyproject.toml",
        rf'(?m)^version = "{SEMVER}"$',
        f'version = "{binding_version}"',
        1,
    )
    for relative in ("bindings/python/setup.py", "bindings/python/src/zlink/_native/_native_loader.py"):
        sync.regex(relative, rf"(?<![0-9.]){SEMVER}(?![0-9.])", core_version, 1)
    sync.regex(
        "bindings/rust/Cargo.toml",
        rf'(\[package\]\nname = "zlink"\nversion = "){SEMVER}(")',
        rf"\g<1>{binding_version}\2",
        1,
    )
    sync.regex(
        "bindings/rust/Cargo.lock",
        rf'(\[\[package\]\]\nname = "zlink"\nversion = "){SEMVER}(")',
        rf"\g<1>{binding_version}\2",
        1,
    )
    for relative in (
        "bindings/rust/perf/single/Cargo.lock",
        "bindings/rust/perf/multi/Cargo.lock",
    ):
        sync.regex(
            relative,
            rf'(\[\[package\]\]\nname = "zlink"\nversion = "){SEMVER}(")',
            rf"\g<1>{binding_version}\2",
            1,
        )
    sync.regex(
        "bindings/java/tests/run_tests.sh",
        rf"Core {SEMVER} install prefix",
        f"Core {core_version} install prefix",
        1,
    )

    sync.regex(
        "scripts/local-package/dotnet/fixtures/public-consumer/PublicConsumer.csproj",
        rf'(<PackageReference Include="Systems\.Zlink" Version="){SEMVER}(" />)',
        rf"\g<1>{binding_version}\2",
        1,
    )
    sync.regex(
        "scripts/local-package/node/fixtures/public-consumer/package.json",
        rf'("@zlink-systems/zlink"\s*:\s*"){SEMVER}(")',
        rf"\g<1>{binding_version}\2",
        1,
    )

    sync.regex(
        "framework/languages/cpp/CMakeLists.txt",
        rf'(set\(ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION "){SEMVER}(" CACHE STRING)',
        rf"\g<1>{binding_version}\2",
        1,
    )
    sync.regex(
        "framework/languages/cpp/CMakeLists.txt",
        rf'(set\(ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION "){SEMVER}(" CACHE STRING)',
        rf"\g<1>{core_version}\2",
        1,
    )
    sync.regex(
        "framework/languages/cpp/samples/sample-build-common.sh",
        rf'(?m)^(\s*local cpp_version="){SEMVER}("\s*)$',
        rf"\g<1>{binding_version}\2",
        1,
    )
    sync.regex(
        "framework/languages/cpp/samples/sample-build-common.sh",
        rf'(?m)^(\s*local core_version="){SEMVER}("\s*)$',
        rf"\g<1>{core_version}\2",
        1,
    )
    sync.regex(
        "framework/languages/cpp/tests/Zlink.Framework.PackageTests/stream_connector_consumer.cmake",
        rf"libzlink\.so\.{SEMVER}",
        f"libzlink.so.{core_version}",
        1,
    )
    sync.regex(
        "framework/languages/dotnet/Directory.Packages.props",
        rf"(<ZLinkBindingsPackageVersion[^>]*>){SEMVER}(</ZLinkBindingsPackageVersion>)",
        rf"\g<1>{binding_version}\2",
        1,
    )
    for relative in (
        "framework/languages/dotnet/contract/packages/Zlink.Framework.package.txt",
        "framework/languages/dotnet/contract/packages/Zlink.Framework.AspNetCore.package.txt",
    ):
        sync.regex(
            relative,
            rf"(id=Systems\.Zlink version=){SEMVER}",
            rf"\g<1>{binding_version}",
            1,
        )
    sync.regex(
        "framework/languages/java/gradle/libs.versions.toml",
        rf'(?m)^zlinkBindings = "{SEMVER}"$',
        f'zlinkBindings = "{binding_version}"',
        1,
    )
    sync.regex(
        "framework/languages/java/e2e/SubmitAdmission/Role/build.gradle.kts",
        rf'(\.orElse\("){SEMVER}("\))',
        rf"\g<1>{binding_version}\2",
        1,
    )
    sync.regex(
        "framework/languages/java/e2e/SubmitAdmission/run_e2e.sh",
        rf"Core {SEMVER} package",
        f"Core {core_version} package",
        1,
    )
    for relative, expected in (
        ("framework/languages/node/package.json", 1),
        ("framework/languages/node/packages/framework/package.json", 1),
        ("framework/languages/node/packages/framework-locations-redis/package.json", 1),
    ):
        sync.transform(
            relative,
            lambda source, expected=expected: update_framework_node_dependency(
                source, binding_version, expected
            ),
        )
    sync.transform(
        "framework/languages/node/package-lock.json",
        lambda source: update_framework_node_lock(source, binding_version),
    )
    sync.regex(
        "framework/languages/node/test/contract/fixtures/node-public-contract.json",
        rf'("bindingVersion"\s*:\s*"){SEMVER}(")',
        rf"\g<1>{binding_version}\2",
        1,
    )
    return core_version, binding_version, sync.changed


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="rewrite managed values")
    mode.add_argument("--check", action="store_true", help="fail if managed values differ")
    args = parser.parse_args()
    try:
        core_version, binding_version, changed = synchronize(args.write)
    except (OSError, SyncError) as error:
        print(f"version sync failed: {error}", file=sys.stderr)
        return 1
    if args.check and changed:
        print(
            "Core/binding versions must be synchronized "
            f"(Core={core_version}, bindings={binding_version}):",
            file=sys.stderr,
        )
        for path in changed:
            print(f"  {path.relative_to(REPO_ROOT)}", file=sys.stderr)
        return 1
    action = "synchronized" if args.write else "verified"
    print(
        f"Core {core_version}; binding packages {binding_version} {action} "
        f"({len(changed)} changed file(s))"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
