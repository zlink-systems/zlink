import hashlib
import os
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class RuntimeInfo:
    path: Path
    sha256: str


def _require_fresh_native_extension(native_dir):
    """Official perf runs use the in-place C extension built from the current sources."""
    built = sorted(native_dir.glob("_zlink_native*.so")) + sorted(native_dir.glob("_zlink_native*.pyd"))
    if not built:
        raise RuntimeError(f"zlink native extension is missing in {native_dir}")
    sources = [path for path in (native_dir / "_zlink_native.c", native_dir / "hotpath.h")
               if path.is_file()]
    if not sources:
        raise RuntimeError(f"zlink native extension source is missing in {native_dir}")
    newest_source = max(path.stat().st_mtime_ns for path in sources)
    if min(path.stat().st_mtime_ns for path in built) < newest_source:
        raise RuntimeError(f"zlink native extension is older than its source: {built[0]}")


def require_binding_runtime(source_dir):
    source_dir = source_dir.resolve()
    source_path = str(source_dir)
    if source_path in sys.path:
        sys.path.remove(source_path)
    sys.path.insert(0, source_path)

    try:
        import zlink

        module_path = Path(zlink.__file__).resolve()
        if (source_dir / "zlink") not in module_path.parents:
            raise RuntimeError(f"zlink was imported outside the source package: {module_path}")
        native_dir = source_dir / "zlink" / "_native"
        _require_fresh_native_extension(native_dir)
        # zlink imports its C extension itself; check that the loaded module is the in-place build.
        loaded = sys.modules.get("zlink._native._zlink_native")
        if loaded is None or native_dir not in Path(loaded.__file__).resolve().parents:
            raise RuntimeError(f"zlink native extension was not loaded from {native_dir}")
        zlink.version()
    except (ImportError, OSError, RuntimeError) as exc:
        raise SystemExit(
            f"The source-tree Python binding runtime is required for perf runs ({exc}). "
            "Run `python3 setup.py build_ext --inplace --force` in bindings/python."
        ) from exc


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
