#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CORE_MANIFEST=""
PACKAGE_VERSION=""
OUTPUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-manifest) CORE_MANIFEST="${2:-}"; shift 2 ;;
    --package-version) PACKAGE_VERSION="${2:-}"; shift 2 ;;
    --output) OUTPUT="${2:-}"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 --core-manifest FILE --package-version X.Y.Z --output FILE"
      exit 0
      ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -f "$CORE_MANIFEST" ]] || { echo "Core manifest not found: $CORE_MANIFEST" >&2; exit 1; }
[[ -n "$PACKAGE_VERSION" && -n "$OUTPUT" ]] || { echo "Core manifest, package version and output are required" >&2; exit 2; }

mkdir -p "$(dirname "$OUTPUT")"
REPO_DIR="$REPO_DIR" CORE_MANIFEST="$CORE_MANIFEST" PACKAGE_VERSION="$PACKAGE_VERSION" OUTPUT="$OUTPUT" python3 <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import subprocess

repo = pathlib.Path(os.environ["REPO_DIR"]).resolve()
core_manifest = pathlib.Path(os.environ["CORE_MANIFEST"]).resolve()
source_root = repo / "bindings" / "python"
output = pathlib.Path(os.environ["OUTPUT"]).resolve()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rel(path):
    return path.relative_to(repo).as_posix()


def include(path):
    name = rel(path)
    parts = pathlib.PurePosixPath(name).parts
    if "__pycache__" in parts or ".pytest_cache" in parts or "build" in parts:
        return False
    if any(part.endswith(".egg-info") for part in parts):
        return False
    if name.startswith("bindings/python/src/zlink/native/"):
        return False
    return path.suffix not in {".so", ".pyc", ".pyd", ".dylib", ".dll"}


source_paths = [
    source_root / name
    for name in ("README.md", "pyproject.toml", "pyrightconfig.json", "setup.py")
]
source_paths.extend(
    root
    for root in (
        source_root / "src" / "zlink",
        source_root / "tests",
        source_root / "samples",
        source_root / "perf",
    )
)
files = [
    {"path": rel(path), "sha256": sha256(path), "mode": stat.S_IMODE(path.stat().st_mode)}
    for root in source_paths
    for path in ([root] if root.is_file() else sorted(root.rglob("*")))
    if path.is_file() and include(path)
]
aggregate_input = json.dumps(files, ensure_ascii=False, separators=(",", ":"))
aggregate = hashlib.sha256(aggregate_input.encode("utf-8")).hexdigest()

direct_paths = [
    pathlib.Path("bindings/doc/plan/python-core-11-update.ko.md"),
    pathlib.Path("bindings/doc/plan/log/python/2026-08-03-posd-ddd-review.ko.md"),
    pathlib.Path("scripts/local-package/bindings-candidate/build-wsl.sh"),
    pathlib.Path("scripts/local-package/bindings-candidate/create-python-source-manifest.sh"),
]
for root in (repo / "bindings/doc/spec/python", repo / "bindings/doc/guide/python"):
    direct_paths.extend(path.relative_to(repo) for path in sorted(root.rglob("*")) if path.is_file())

direct_inputs = []
for path in sorted(set(direct_paths), key=lambda item: item.as_posix()):
    absolute = repo / path
    if not absolute.is_file():
        raise SystemExit(f"Missing Python direct input: {path}")
    direct_inputs.append({"path": path.as_posix(), "sha256": sha256(absolute)})

manifest = {
    "schema": 1,
    "language": "python",
    "packageVersion": os.environ["PACKAGE_VERSION"],
    "sourceRevision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
    "sourceRoot": "bindings/python",
    "files": files,
    "aggregateSha256": aggregate,
    "directInputs": [
        {"path": rel(core_manifest), "sha256": sha256(core_manifest)},
        *direct_inputs,
    ],
    "packageScript": {
        "path": "scripts/local-package/bindings-candidate/build-wsl.sh",
        "sha256": sha256(repo / "scripts/local-package/bindings-candidate/build-wsl.sh"),
    },
}
output.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Python source manifest: {output}")
print(f"Python source aggregate SHA-256: {aggregate}")
PY
