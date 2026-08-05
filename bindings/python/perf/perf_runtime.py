import hashlib
import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class RuntimeInfo:
    path: Path
    sha256: str


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _resolve_explicit_runtime():
    value = os.environ.get("ZLINK_LIBRARY_PATH")
    if not value:
        raise SystemExit(
            "ZLINK_LIBRARY_PATH is required for Python perf; "
            "pass the approved Core or wheel runtime explicitly"
        )
    try:
        path = Path(value).expanduser().resolve(strict=True)
    except OSError as exc:
        raise SystemExit(f"Python perf runtime is not accessible: {value}") from exc
    if not path.is_file():
        raise SystemExit(f"Python perf runtime is not a file: {path}")
    return path


def _reject_stale_source_runtime(path, repo_root):
    core_build_lib = (repo_root / "core" / "build" / "lib").resolve()
    if path.parent != core_build_lib:
        return
    runtime_mtime = path.stat().st_mtime
    for source_root in (repo_root / "core" / "src", repo_root / "core" / "include"):
        if not source_root.exists():
            continue
        for source in source_root.rglob("*"):
            if source.is_file() and source.stat().st_mtime > runtime_mtime:
                raise SystemExit(
                    "Error: stale core runtime detected for bindings/python/perf.\n"
                    f"  runtime: {path}\n"
                    f"  newer source: {source}\n"
                    "Rebuild the Core runtime before running Python perf."
                )


def configure_runtime(env, repo_root):
    path = _resolve_explicit_runtime()
    _reject_stale_source_runtime(path, repo_root)
    info = RuntimeInfo(path=path, sha256=_sha256(path))
    env["ZLINK_LIBRARY_PATH"] = str(info.path)
    lib_dir = str(info.path.parent)
    env["LD_LIBRARY_PATH"] = (
        f"{lib_dir}:{env['LD_LIBRARY_PATH']}"
        if env.get("LD_LIBRARY_PATH")
        else lib_dir
    )
    print(f"Perf runtime libzlink: {info.path}", flush=True)
    print(f"Perf runtime sha256: {info.sha256}", flush=True)
    return info
