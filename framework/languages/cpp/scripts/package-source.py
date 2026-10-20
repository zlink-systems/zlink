#!/usr/bin/env python3
"""Build the framework C++ source archive from the HEAD commit.

Usage: package-source.py <output-dir>

Writes <output-dir>/zlink-framework-cpp-<VERSION>.tar.gz and prints its path.
The archive holds framework/languages/cpp (as the top-level directory),
framework/runtime (as runtime/), framework/LICENSE and the three
static-archive scripts of core/tools (as tools/).

This is the only producer of the archive. scripts/dev/release-check.sh
--write framework cpp runs it on the release branch to record the archive's
hashes in the vcpkg port and Conan recipe, and framework-release.yml runs the
same command on the tag and publishes the archive only when those recorded
hashes are unchanged (#1941). Both runs must yield the same bytes, so:

- framework/languages/cpp/packaging/ is left out. It holds the Conan recipe
  that records this archive's hash; including it would make the archive
  depend on its own hash. Builds from the archive do not read packaging/.
- Entries are sorted by name and carry a fixed mtime, owner and group 0 and
  no user or group names; gzip stores no file name and no timestamp.
- Only committed content is archived (git archive HEAD), and the archived
  paths must have no uncommitted changes.
"""

import gzip
import io
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path

# A constant, not the commit time: the recipe commit that records this
# archive's hash changes HEAD but must not change the archive.
ARCHIVE_MTIME = 946684800  # 2000-01-01T00:00:00Z

CPP_DIR = "framework/languages/cpp"
EXCLUDED_DIR = f"{CPP_DIR}/packaging"
# repository path -> path inside the archive's top-level directory
MAPPED_PATHS = (
    (CPP_DIR, ""),
    ("framework/runtime", "runtime"),
    ("framework/LICENSE", "LICENSE"),
    # The static archives are restricted to the zlink:: surface by Core's
    # tools (#2133); a checkout reads them in core/tools, the archive carries
    # the same three scripts next to CMakeLists.txt.
    ("core/tools/hide_static_archive_internals.sh", "tools/hide_static_archive_internals.sh"),
    ("core/tools/static_archive_public_symbols.sh", "tools/static_archive_public_symbols.sh"),
    ("core/tools/check_static_archive_exports.sh", "tools/check_static_archive_exports.sh"),
)
REQUIRED_FILES = (
    "CMakeLists.txt",
    "runtime/protocol/generated/cpp/service_wire_constants.hpp",
    "runtime/protocol/generated/cpp/service_wire_pilot_codec.hpp",
    "tools/hide_static_archive_internals.sh",
    "LICENSE",
)


def fail(message):
    print(f"package-source: {message}", file=sys.stderr)
    sys.exit(1)


def git(root, *args):
    result = subprocess.run(["git", *args], cwd=root, capture_output=True)
    if result.returncode != 0:
        fail(f"git {' '.join(args)} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def archive_path(repository_path):
    """Path inside the archive (without the top-level directory), or None."""
    if repository_path == EXCLUDED_DIR or repository_path.startswith(EXCLUDED_DIR + "/"):
        return None
    for source, target in MAPPED_PATHS:
        if repository_path == source:
            return target
        if repository_path.startswith(source + "/"):
            rest = repository_path[len(source) + 1:]
            return f"{target}/{rest}" if target else rest
    return None


def main():
    if len(sys.argv) != 2:
        fail("usage: package-source.py <output-dir>")
    output_dir = Path(sys.argv[1])
    root = Path(git(Path(__file__).resolve().parent, "rev-parse", "--show-toplevel").decode().strip())
    sources = [source for source, _ in MAPPED_PATHS]

    pathspecs = [*sources, f":(exclude){EXCLUDED_DIR}"]
    if subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *pathspecs], cwd=root).returncode != 0:
        fail("archived paths have uncommitted changes; commit them first (the archive is built from HEAD)")

    # The codec generator is the one --check entry point for every generated
    # service-wire output (framework/runtime/protocol/README.ko.md).
    subprocess.run(
        ["node", "framework/runtime/protocol/generate-service-wire-codecs.mjs", "--check"],
        cwd=root, check=True,
    )

    version_file = git(root, "show", f"HEAD:{CPP_DIR}/VERSION").decode()
    version = re.search(r"(?m)^ZLINK_FRAMEWORK_VERSION=([0-9]+\.[0-9]+\.[0-9]+)\s*$", version_file)
    if version is None:
        fail(f"{CPP_DIR}/VERSION has no ZLINK_FRAMEWORK_VERSION=X.Y.Z")
    top = f"zlink-framework-cpp-{version.group(1)}"

    entries = {}
    with tarfile.open(fileobj=io.BytesIO(git(root, "archive", "--format=tar", "HEAD", "--", *sources)),
                      mode="r:") as source_tar:
        for member in source_tar:
            relative = archive_path(member.name.rstrip("/"))
            if relative is None or not (member.isfile() or member.isdir() or member.issym()):
                continue
            name = f"{top}/{relative}" if relative else top
            info = tarfile.TarInfo(name)
            info.type = member.type
            info.mode = member.mode
            info.linkname = member.linkname
            info.size = member.size if member.isfile() else 0
            info.mtime = ARCHIVE_MTIME
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            data = source_tar.extractfile(member).read() if member.isfile() else None
            entries[name] = (info, data)

    missing = [path for path in REQUIRED_FILES if f"{top}/{path}" not in entries]
    if missing:
        fail(f"archive is incomplete: missing {', '.join(missing)}")

    tar_bytes = io.BytesIO()
    with tarfile.open(fileobj=tar_bytes, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for name in sorted(entries):
            info, data = entries[name]
            archive.addfile(info, io.BytesIO(data) if data is not None else None)
    gz_bytes = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=gz_bytes, compresslevel=9, mtime=0) as gz:
        gz.write(tar_bytes.getvalue())

    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / f"{top}.tar.gz"
    partial = output.with_name(output.name + ".partial")
    partial.write_bytes(gz_bytes.getvalue())
    os.replace(partial, output)
    print(output)


if __name__ == "__main__":
    main()
