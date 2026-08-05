#!/usr/bin/env python3
"""Public surface contract gate for Core 11 raw runtime.

Verifies that the installed public header closure and the built shared
library expose exactly the C surface defined by the formal spec under
core/doc/spec/core/.

Checks:
  1. Korean and English formal spec files carry identical C blocks.
  2. Header closure reachable from core/include/zlink.h declares every
     formal function, and no function outside the formal set.
  3. Formal types, enum types, enumerators, struct fields and macros are
     all present in the header closure.
  4. Dynamic exports of libzlink match the formal function set exactly
     (no headerless internal exports).
  5. Debian, RPM and NuGet metadata match the CMake version and SONAME.

Usage: check_public_surface.py <repo_root> <libzlink_path>
Exit code 0 on success, 1 on contract violation.
"""

import pathlib
import re
import subprocess
import sys

KINDS = ("FUNC", "TYPE", "ENUM_TYPE", "ENUMERATOR", "FIELD", "MACRO")

# Build-infrastructure macros that are not part of the public contract
# and are therefore ignored on the header side.
INFRA_MACROS = re.compile(r"^ZLINK_.*_H_INCLUDED$|^ZLINK_EXPORT$|^ZLINK_DEFINED_STDINT$")

# Retained public macros that the formal spec documents in prose tables
# rather than inside C blocks.  They are allowed in headers even though
# the formal C-block parse does not produce them.
TABLE_ONLY_MACROS = {
    "ZLINK_DONTWAIT",
    "ZLINK_MSG_METADATA_KEY_USER_MIN",
    "ZLINK_MSG_METADATA_VALUE_MAX",
    "ZLINK_NULL",
    "ZLINK_PLAIN",
}


def parse_c_surface(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    out = {kind: set() for kind in KINDS}
    for declaration in re.findall(r"ZLINK_EXPORT\s+([^;]+);", text, re.S):
        found = re.search(r"\b(zlink_[A-Za-z0-9_]+)\s*\(", declaration)
        if found:
            out["FUNC"].add(found.group(1))
    for found in re.finditer(
        r"typedef\s+enum\s+\w+\s*\{(.*?)\}\s*(zlink_\w+)\s*;", text, re.S
    ):
        out["ENUM_TYPE"].add(found.group(2))
        out["ENUMERATOR"].update(
            re.findall(r"\b(ZLINK_[A-Z0-9_]+)\s*(?==|,)", found.group(1))
        )
    for found in re.finditer(r"(?<!typedef\s)enum\s*\{(.*?)\}\s*;", text, re.S):
        out["ENUMERATOR"].update(
            re.findall(r"\b(ZLINK_[A-Z0-9_]+)\s*(?==|,)", found.group(1))
        )
    for found in re.finditer(
        r"typedef\s+struct(?:\s+\w+)?\s*\{(.*?)\}\s*(zlink_\w+)\s*;", text, re.S
    ):
        body, owner = found.group(1), found.group(2)
        out["TYPE"].add(owner)
        for declaration in body.split(";")[:-1]:
            field = re.search(
                r"([A-Za-z_]\w*)\s*(?:\[[^\]]+\])?\s*$", declaration.strip(), re.S
            )
            if field:
                out["FIELD"].add(f"{owner}.{field.group(1)}")
    without_blocks = re.sub(
        r"typedef\s+(?:enum|struct)(?:\s+\w+)?\s*\{.*?\}\s*zlink_\w+\s*;",
        " ",
        text,
        flags=re.S,
    )
    for statement in re.findall(r"\btypedef\b.*?;", without_blocks, re.S):
        found = re.search(r"\(\s*\*?\s*(zlink_\w+)\s*\)\s*\(", statement)
        if not found:
            found = re.search(r"\b(zlink_\w+)\s*;$", statement)
        if found:
            out["TYPE"].add(found.group(1))
    out["MACRO"].update(re.findall(r"^\s*#\s*define\s+(ZLINK_\w+)", text, re.M))
    return out


def header_closure(root, entry="zlink.h"):
    include_root = root / "core" / "include"
    seen = []
    queue = [include_root / entry]
    visited = set()
    while queue:
        path = queue.pop()
        if path in visited or not path.exists():
            continue
        visited.add(path)
        seen.append(path)
        text = path.read_text()
        for rel in re.findall(r'#\s*include\s+["<]([^">]+)[">]', text):
            candidate = include_root / rel
            if candidate.exists():
                queue.append(candidate)
    return seen


def c_blocks(path):
    text = path.read_text()
    fence = chr(96) * 3
    return "\n".join(re.findall(rf"^{fence}c\s*\n(.*?)^{fence}\s*$", text, re.M | re.S))


def check_packaging_metadata(root, failures):
    cmake = (root / "core" / "CMakeLists.txt").read_text()
    version_match = re.search(r"project\s*\(\s*zlink\s+VERSION\s+([0-9.]+)", cmake)
    soname_match = re.search(r'\bSOVERSION\s+"([0-9]+)"', cmake)
    if not version_match or not soname_match:
        failures.append("cannot derive package version or SOVERSION from core/CMakeLists.txt")
        return
    version = version_match.group(1)
    soname = soname_match.group(1)
    version_path = version.replace(".", "_")

    expected_text = {
        "core/packaging/debian/control": (
            f"Package: libzlink{soname}",
            f"Package: libzlink{soname}-dev",
            f"Package: libzlink{soname}-dbg",
        ),
        "core/packaging/debian/changelog": (f"zlink ({version}-0.1)",),
        "core/packaging/debian/zlink.dsc": (
            f"Binary: libzlink{soname}, libzlink{soname}-dev, libzlink{soname}-dbg",
            f"Version: {version}-0.1",
        ),
        "core/packaging/debian/rules": (f"--dbg-package=libzlink{soname}-dbg",),
        "core/packaging/redhat/zlink.spec": (
            f"%define lib_name libzlink{soname}",
            f"Version:       {version}",
        ),
        "core/packaging/nuget/package.config": (
            f'version = "{version}"',
            f'pathversion="{version_path}"',
        ),
        "core/packaging/nuget/package.nuspec": (
            f"<version>{version}</version>",
            version_path,
        ),
        "core/packaging/nuget/package.targets": (version_path,),
    }
    for relative, needles in expected_text.items():
        path = root / relative
        if not path.exists():
            failures.append(f"packaging metadata missing: {relative}")
            continue
        text = path.read_text()
        for needle in needles:
            if needle not in text:
                failures.append(f"packaging metadata mismatch: {relative} lacks {needle!r}")

    debian = root / "core" / "packaging" / "debian"
    expected_files = {
        f"libzlink{soname}.install",
        f"libzlink{soname}.docs",
        f"libzlink{soname}-dev.install",
        f"libzlink{soname}-dev.manpages",
    }
    missing = sorted(name for name in expected_files if not (debian / name).exists())
    if missing:
        failures.append(f"Debian ABI package files missing: {missing}")
    stale = sorted(path.name for path in debian.glob("libzlink[0-9]*")
                   if path.name not in expected_files)
    if stale:
        failures.append(f"stale Debian ABI package files: {stale}")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    root = pathlib.Path(sys.argv[1])
    lib = pathlib.Path(sys.argv[2])
    failures = []

    spec_dir = root / "core" / "doc" / "spec" / "core"
    en_docs = sorted(
        p for p in spec_dir.rglob("*.md") if not p.name.endswith(".ko.md")
    )
    formal = {kind: set() for kind in KINDS}
    for en in en_docs:
        ko = en.with_name(en.name[: -len(".md")] + ".ko.md")
        en_c = c_blocks(en)
        if ko.exists() and c_blocks(ko) != en_c:
            failures.append(f"ko/en C block mismatch: {en.relative_to(root)}")
        parsed = parse_c_surface(en_c)
        for kind in KINDS:
            formal[kind].update(parsed[kind])

    root_headers = {kind: set() for kind in KINDS}
    root_closure = header_closure(root)
    for path in root_closure:
        parsed = parse_c_surface(path.read_text())
        for kind in KINDS:
            root_headers[kind].update(parsed[kind])

    headers = {kind: set(root_headers[kind]) for kind in KINDS}
    headers["MACRO"] = {
        m for m in headers["MACRO"] if not INFRA_MACROS.match(m)
    }

    missing_funcs = formal["FUNC"] - headers["FUNC"]
    extra_funcs = headers["FUNC"] - formal["FUNC"]
    if missing_funcs:
        failures.append(f"header missing formal functions ({len(missing_funcs)}): "
                        f"{sorted(missing_funcs)[:8]} ...")
    if extra_funcs:
        failures.append(f"header declares non-formal functions ({len(extra_funcs)}): "
                        f"{sorted(extra_funcs)[:8]} ...")

    for kind in ("TYPE", "ENUM_TYPE", "ENUMERATOR", "FIELD", "MACRO"):
        missing = formal[kind] - headers[kind]
        if missing:
            failures.append(f"header missing formal {kind} ({len(missing)}): "
                            f"{sorted(missing)[:8]} ...")

    check_packaging_metadata(root, failures)

    if lib.exists():
        nm = subprocess.run(
            ["nm", "-D", "--defined-only", str(lib)],
            capture_output=True, text=True, check=True,
        )
        exports = {
            line.split()[-1]
            for line in nm.stdout.splitlines()
            if line.split() and line.split()[-1].startswith("zlink_")
        }
        missing_exports = formal["FUNC"] - exports
        extra_exports = exports - formal["FUNC"]
        if missing_exports:
            failures.append(f"library missing formal exports ({len(missing_exports)}): "
                            f"{sorted(missing_exports)[:8]} ...")
        if extra_exports:
            failures.append(f"library exports non-formal symbols ({len(extra_exports)}): "
                            f"{sorted(extra_exports)[:8]} ...")
    else:
        failures.append(f"library not found: {lib}")

    if failures:
        print("PUBLIC SURFACE CONTRACT: FAIL")
        for failure in failures:
            print(" -", failure)
        return 1
    print("PUBLIC SURFACE CONTRACT: PASS")
    print(f"functions={len(formal['FUNC'])} exports match, "
          f"removed identifiers absent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
