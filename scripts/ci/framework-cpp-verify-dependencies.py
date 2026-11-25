"""Compare the vcpkg installation used by CI with the Conan prebuilt locks."""

import argparse
import json
from pathlib import Path


FRAMEWORK = Path(__file__).resolve().parents[2] / "framework/languages/cpp"
CONAN_TO_VCPKG = {
    "boost": ("boost-asio", "boost-beast"),
    "opentelemetry-cpp": ("opentelemetry-cpp",),
    "hiredis": ("hiredis",),
    "zlib": ("zlib",),
    "lz4": ("lz4",),
    "nlohmann_json": ("nlohmann-json",),
}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def installed_packages(status_path, triplet):
    packages = {}
    for block in status_path.read_text(encoding="utf-8").split("\n\n"):
        fields = dict(line.split(": ", 1) for line in block.splitlines() if ": " in line)
        if (fields.get("Architecture") == triplet and "Feature" not in fields
                and fields.get("Status") == "install ok installed"):
            packages[fields["Package"]] = (
                fields["Version"], int(fields.get("Port-Version", "0"))
            )
    return packages


def conan_lock_versions():
    packaging = FRAMEWORK / "packaging"
    platforms = read_json(packaging / "prebuilt-platforms.json")["include"]
    versions = {}
    for entry in platforms:
        platform = entry["platform"]
        lock = read_json(packaging / "conan" / f"conan.{platform}.lock")
        versions[platform] = dict(ref.split("#", 1)[0].split("/", 1) for ref in lock["requires"])
    return versions


def verify_parity(installed, locks):
    failures = []
    print("| Conan dependency | Conan locks | vcpkg target triplet |")
    print("|---|---|---|")
    for dependency, ports in CONAN_TO_VCPKG.items():
        expected = {platform: versions.get(dependency) for platform, versions in locks.items()}
        port_names = set(ports)
        if dependency == "boost":
            port_names.update(
                name for name in installed if name == "boost" or name.startswith("boost-")
            )
        actual = {port: installed.get(port, (None, 0))[0] for port in sorted(port_names)}

        lock_versions = set(expected.values())
        vcpkg_versions = set(actual.values())
        conan_column = (next(iter(lock_versions)) if len(lock_versions) == 1
                        else ", ".join(f"{platform}={version}" for platform, version in expected.items()))
        vcpkg_column = (next(iter(vcpkg_versions)) if len(vcpkg_versions) == 1
                        else ", ".join(f"{port}={version}" for port, version in actual.items()))
        print(f"| {dependency} | {conan_column} | {vcpkg_column} |")
        if None in lock_versions or len(lock_versions) != 1 or lock_versions != vcpkg_versions:
            failures.append(dependency)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("status", type=Path)
    parser.add_argument("triplet")
    parser.add_argument("--with-etcd", action="store_true", help="require the vcpkg gRPC override")
    args = parser.parse_args()

    installed = installed_packages(args.status, args.triplet)
    manifest_path = FRAMEWORK / "vcpkg.json"
    required = {"abseil", "protobuf", "yyjson"}
    if args.with_etcd:
        required.add("grpc")

    failures = []
    for pin in read_json(manifest_path)["overrides"]:
        name = pin["name"]
        if name in installed or name in required:
            expected = (pin["version"], pin.get("port-version", 0))
            actual = installed.get(name)
            print(f"{name}:{args.triplet} installed={actual} vcpkg.json={expected}")
            if actual != expected:
                failures.append(name)

    mismatches = verify_parity(installed, conan_lock_versions())
    if failures:
        print(f"vcpkg installation differs from vcpkg.json for {', '.join(failures)}")
    if mismatches:
        print(f"Conan locks and vcpkg differ for {', '.join(mismatches)}")
    return int(bool(failures or mismatches))


if __name__ == "__main__":
    raise SystemExit(main())
