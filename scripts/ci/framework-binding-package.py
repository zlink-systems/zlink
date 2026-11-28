#!/usr/bin/env python3
"""Prepare the exact Framework binding package, or build it from the checkout."""

import argparse
import base64
import hashlib
from http.client import HTTPException
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen
import xml.etree.ElementTree as ET
import zipfile

from framework_timing import timed


BINDING_INPUTS = ("bindings/", "core/")
CPP_CACHE_INPUTS = ("VERSION", "LICENSE", "bindings/cpp", "core",
                    "scripts/ci/framework-cpp-build-binding.sh",
                    "scripts/local-package/cpp", "scripts/local-package/build-windows.ps1")
REPO = "zlink-systems/zlink"


def git(*args):
    return subprocess.check_output(("git", "--no-optional-locks", *args), stderr=subprocess.PIPE)


def binding_inputs_changed(base):
    # Disable rename detection so both ends of a move are considered. The
    # working-tree checks also cover staged and untracked local changes.
    return bool(
        git("diff", "--name-only", "--no-renames", "-z", f"{base}...HEAD", "--", *BINDING_INPUTS)
        or git("status", "--porcelain", "--untracked-files=all", "-z", "--", *BINDING_INPUTS)
    )


def version(file, key):
    return dict(line.split("=", 1) for line in Path(file).read_text().splitlines())[key]


def download(url):
    with urlopen(url, timeout=30) as response:
        return response.read()


def digest(data, algorithm="sha256"):
    return hashlib.new(algorithm, data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def maven_package(destination, binding, core):
    name = f"zlink-{binding}"
    url = f"https://repo.maven.apache.org/maven2/systems/zlink/zlink/{binding}/{name}"
    pom = download(f"{url}.pom")
    module = download(f"{url}.module")
    for extension, data in (("pom", pom), ("module", module)):
        checksum = download(f"{url}.{extension}.sha1").decode().strip()
        require(digest(data, "sha1") == checksum, f"Maven {extension} digest mismatch")
    project = ET.fromstring(pom)
    require(project.findtext("{*}version") == binding
            and project.findtext("{*}properties/{*}zlink.core.version") == core,
            "Maven binding or Core version mismatch")
    metadata = json.loads(module)
    require(metadata["component"]["version"] == binding, "Maven module version mismatch")
    checksums = {file["sha256"] for variant in metadata["variants"]
                 for file in variant.get("files", ()) if file["name"] == f"{name}.jar"}
    jar = download(f"{url}.jar")
    require(checksums == {digest(jar)}, "Maven JAR digest mismatch")
    package = destination / "maven/systems/zlink/zlink" / binding
    for extension, data in (("jar", jar), ("pom", pom), ("module", module)):
        write_package(package / f"{name}.{extension}", data)


def npm_package(destination, binding, core):
    meta = json.loads(download(f"https://registry.npmjs.org/@zlink-systems%2fzlink/{binding}"))
    require(meta["name"] == "@zlink-systems/zlink" and meta["version"] == binding,
            "npm binding version mismatch")
    data = download(f"https://registry.npmjs.org/@zlink-systems/zlink/-/zlink-{binding}.tgz")
    checksum = base64.b64encode(hashlib.sha512(data).digest()).decode()
    require(meta["dist"]["integrity"] == f"sha512-{checksum}", "npm tarball digest mismatch")
    platform = subprocess.check_output(
        ["node", "-p", "process.platform + '-' + process.arch"], text=True).strip()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        manifest = json.load(archive.extractfile("package/package.json"))
        provenance = json.load(archive.extractfile("package/provenance/core-package-provenance.json"))
        require(manifest["version"] == binding and provenance["version"] == core,
                "npm binding or Core version mismatch")
        require(archive.getmember(f"package/prebuilds/{platform}/zlink.node").isfile(),
                f"npm native prebuild missing for {platform}")
    write_package(destination / "npm" / f"zlink-systems-zlink-{binding}.tgz", data)


def nuget_package(destination, binding, core):
    # NuGet v2 exposes the SHA-512 for the exact nupkg served by the v3 flat container.
    entry = ET.fromstring(download(
        f"https://www.nuget.org/api/v2/Packages(Id='Zlink',Version='{binding}')"))
    properties = entry.find("{*}properties")
    require(properties.findtext("{*}Version") == binding, "NuGet metadata version mismatch")
    lower = binding.lower()
    data = download(f"https://api.nuget.org/v3-flatcontainer/zlink/{lower}/zlink.{lower}.nupkg")
    checksum = base64.b64encode(hashlib.sha512(data).digest()).decode()
    require(properties.findtext("{*}PackageHashAlgorithm") == "SHA512"
            and properties.findtext("{*}PackageHash") == checksum, "NuGet package digest mismatch")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        nuspec = ET.fromstring(archive.read("Zlink.nuspec"))
        provenance = json.loads(archive.read("provenance/core-package-provenance.json"))
        require(nuspec.findtext("{*}metadata/{*}id") == "Zlink"
                and nuspec.findtext("{*}metadata/{*}version") == binding
                and provenance["version"] == core, "NuGet binding or Core version mismatch")
    write_package(destination / f"Zlink.{binding}.nupkg", data)


def write_package(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    # Replace existing output, including a symlink to a shared development cache.
    with tempfile.TemporaryDirectory(dir=path.parent) as directory:
        staged = Path(directory) / path.name
        staged.write_bytes(data)
        staged.replace(path)


def cpp_cache_name(binding):
    flags = ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CL", "_CL_",
             "CMAKE_GENERATOR", "CMAKE_TOOLCHAIN_FILE")
    require(not any(os.environ.get(flag) for flag in flags)
            and os.environ.get("CONFIGURATION", "Release") == "Release",
            "custom C++ build flags")
    require(not git("status", "--porcelain", "-z", "--", *CPP_CACHE_INPUTS),
            "C++ package inputs have working-tree changes")
    if os.name == "nt":
        toolchain = os.environ["VCToolsVersion"].strip("\\/")
    else:
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        target = subprocess.check_output([*compiler, "-dumpmachine"]).strip()
        banner = subprocess.check_output([*compiler, "--version"]).splitlines()[0]
        toolchain = digest(target + b"\0" + banner)[:16]
    tree = git("ls-tree", "-z", "HEAD", "--", *CPP_CACHE_INPUTS)
    return (f"framework-cpp-binding-v1-{binding}-{os.environ['RUNNER_OS']}-"
            f"{os.environ['RUNNER_ARCH']}-{toolchain}-{digest(tree)}")


def cpp_files(destination, binding):
    prefix = destination / "install/zlink-cpp" / binding
    library = "lib/zlink_cpp.lib" if os.name == "nt" else "lib/libzlink_cpp.a"
    for name in ("include/zlink.hpp", library):
        require((prefix / name).is_file(), f"C++ package is missing {name}")
    return {path.relative_to(prefix).as_posix(): path.read_bytes()
            for path in prefix.rglob("*") if path.is_file()}


def pack_cpp(destination, binding, name):
    files = cpp_files(destination, binding)
    manifest = {"key": name, "sha256": {path: digest(data) for path, data in files.items()}}
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps(manifest))
        for path, data in files.items():
            archive.writestr(f"package/{path}", data)
    return output.getvalue()


def gh_api(*args):
    return subprocess.check_output(("gh", "api", *args), stderr=subprocess.PIPE, timeout=60)


def fetch_cpp(name):
    result = json.loads(gh_api(f"repos/{REPO}/actions/artifacts?name={name}&per_page=100"))
    candidates = [item for item in result["artifacts"] if not item["expired"]
                  and item["name"] == name and item["workflow_run"]["head_branch"] == "main"
                  and item["workflow_run"]["repository_id"] == item["workflow_run"]["head_repository_id"]]
    require(candidates, "main cache artifact unavailable")
    item = candidates[0]
    data = gh_api(f"repos/{REPO}/actions/artifacts/{item['id']}/zip")
    require(item["digest"] == f"sha256:{digest(data)}", "main cache artifact digest mismatch")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        return archive.read("binding.zip")


def restore_cpp(destination, binding, name, data):
    prefix = destination / "install/zlink-cpp"
    prefix.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=prefix) as directory:
        stage = Path(directory)
        target = stage / "install/zlink-cpp" / binding
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            manifest = json.loads(archive.read("manifest.json"))
            require(manifest["key"] == name, "C++ binding cache key or version mismatch")
            for path, checksum in manifest["sha256"].items():
                output = target / path
                require(output.resolve().is_relative_to(target.resolve()) and "\\" not in path,
                        "invalid C++ binding archive path")
                content = archive.read(f"package/{path}")
                require(digest(content) == checksum, f"C++ binding digest mismatch for {path}")
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(content)
        cpp_files(stage, binding)
        installed = prefix / binding
        if installed.is_symlink():
            installed.unlink()
        elif installed.exists():
            shutil.rmtree(installed)
        target.replace(installed)


def prepare(args):
    binding = version(f"bindings/{args.language}/VERSION", "ZLINK_BINDING_VERSION")
    core = version("VERSION", "LIBZLINK_VERSION")
    for value in (binding, core):
        require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?", value),
                f"invalid package version {value!r}")

    name = None
    try:
        require(not binding_inputs_changed(args.base), "bindings/ or core/ changed")
        # Release metadata checks the embedded Core version against this checkout.
        if args.language == "cpp":
            name = cpp_cache_name(binding)
            local_dir = os.environ.get("ZLINK_LOCAL_BINDING_CACHE")
            local = Path(local_dir) / f"{name}.zip" if local_dir else None
            with timed("C++ binding package fetch and verification"):
                if local and local.exists():
                    try:
                        restore_cpp(args.destination, binding, name, local.read_bytes())
                    except (ValueError, KeyError, RuntimeError, zipfile.BadZipFile):
                        local.unlink()
                        raise
                else:
                    data = fetch_cpp(name)
                    restore_cpp(args.destination, binding, name, data)
                    if local:
                        write_package(local, data)
        else:
            fetch = {"java": maven_package, "node": npm_package, "dotnet": nuget_package}
            with timed(f"{args.language} binding package fetch and verification"):
                fetch[args.language](args.destination, binding, core)
        print(f"{args.language} binding {binding}: reused verified package", flush=True)
    except (OSError, EOFError, ValueError, TypeError, KeyError, AttributeError, RuntimeError,
            ET.ParseError, HTTPException, zipfile.BadZipFile, tarfile.TarError,
            subprocess.SubprocessError) as error:
        reason = " ".join(str(error).splitlines())
        print(f"{args.language} binding {binding}: {reason}; building from checkout", flush=True)
        with timed(f"{args.language} checkout binding package build"):
            subprocess.run(args.build, check=True)

    if args.publish:
        name = name or cpp_cache_name(binding)
        archive = Path(os.environ["RUNNER_TEMP"]) / "framework-cpp-binding/binding.zip"
        write_package(archive, pack_cpp(args.destination, binding, name))
        with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
            print(f"cache-name={name}\ncache-archive={archive}", file=output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--dry-run", action="store_true", help="report the path decision without fetching")
    parser.add_argument("--publish", action="store_true", help="export the verified C++ package for main")
    parser.add_argument("language", choices=("cpp", "java", "node", "dotnet"))
    parser.add_argument("destination", type=Path)
    parser.add_argument("build", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.publish and args.language != "cpp":
        parser.error("--publish requires the C++ binding")
    if args.dry_run:
        choice = "build from checkout" if binding_inputs_changed(args.base) else "try verified package"
        print(f"{args.language} binding: {choice}")
    else:
        if args.build[:1] == ["--"]:
            args.build.pop(0)
        if not args.build:
            parser.error("missing checkout build command")
        prepare(args)


if __name__ == "__main__":
    main()
