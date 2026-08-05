#!/usr/bin/env python3
import argparse
import shutil
import stat
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CPP_PERF_DIR = ROOT / "bindings" / "cpp" / "perf"
RUNTIME_ROOT = CPP_PERF_DIR / ".runtime"
C_STREAM_CLIENT = ROOT / "bindings" / "c" / "build" / "perf" / "perf_stream_client"

SINGLE_MAP = {
    "perf_pair": CPP_PERF_DIR / "single" / "build" / "cpp_perf_pair",
    "perf_pubsub": CPP_PERF_DIR / "single" / "build" / "cpp_perf_pubsub",
    "perf_dealer_dealer": CPP_PERF_DIR / "single" / "build" / "cpp_perf_dealer_dealer",
    "perf_dealer_router": CPP_PERF_DIR / "single" / "build" / "cpp_perf_dealer_router",
    "perf_dealer_router_reqrep": CPP_PERF_DIR / "single" / "build" / "cpp_perf_dealer_router_reqrep",
    "perf_router_router": CPP_PERF_DIR / "single" / "build" / "cpp_perf_router_router",
    "perf_router_router_reqrep": CPP_PERF_DIR / "single" / "build" / "cpp_perf_router_router_reqrep",
}

MULTI_SERVER_MAP = {
    "comp_src_dealer_dealer_server": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_dealer_dealer_server",
    "comp_src_dealer_router_server": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_dealer_router_server",
    "comp_src_dealer_router_reqrep_server": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_dealer_router_reqrep_server",
    "comp_src_router_router_server": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_router_router_server",
    "comp_src_router_router_reqrep_server": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_router_router_reqrep_server",
    "comp_src_pubsub_server": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_pubsub_server",
    "comp_src_stream_server": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_stream_server",
}

MULTI_CLIENT_MAP = {
    "comp_src_dealer_dealer_client": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_dealer_dealer_client",
    "comp_src_dealer_router_client": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_dealer_router_client",
    "comp_src_dealer_router_reqrep_client": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_dealer_router_reqrep_client",
    "comp_src_router_router_client": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_router_router_client",
    "comp_src_router_router_reqrep_client": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_router_router_reqrep_client",
    "comp_src_pubsub_client": CPP_PERF_DIR / "multi" / "build" / "cpp_comp_src_pubsub_client",
}


def reset_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for child in path.iterdir():
        if child.is_dir() and not child.is_symlink():
            shutil.rmtree(child)
        else:
            child.unlink()


def write_executable(path: Path, text: str) -> None:
    path.write_text(text)
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def make_symlink(path: Path, target: Path) -> None:
    path.symlink_to(target)


def prepare_single(runtime_bin: Path) -> None:
    for name, target in SINGLE_MAP.items():
        make_symlink(runtime_bin / name, target)


def prepare_multi(runtime_bin: Path) -> None:
    for name, target in MULTI_SERVER_MAP.items():
        make_symlink(runtime_bin / name, target)
        make_symlink(runtime_bin / target.name, target)
    for name, target in MULTI_CLIENT_MAP.items():
        make_symlink(runtime_bin / name, target)
        make_symlink(runtime_bin / target.name, target)
    make_symlink(runtime_bin / "perf_stream_client", C_STREAM_CLIENT)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", choices=("single", "multi"), required=True)
    parser.add_argument("--runtime-lib", required=True)
    args = parser.parse_args()

    runtime_lib_source = Path(args.runtime_lib).resolve()
    if not runtime_lib_source.is_file():
        raise SystemExit(f"runtime library not found: {runtime_lib_source}")
    runtime_lib_source_dir = runtime_lib_source.parent

    runtime_dir = RUNTIME_ROOT / args.suite
    runtime_bin = runtime_dir / "bin"
    runtime_lib = runtime_dir / "lib"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    reset_dir(runtime_bin)
    if runtime_lib.exists() or runtime_lib.is_symlink():
        if runtime_lib.is_dir() and not runtime_lib.is_symlink():
            shutil.rmtree(runtime_lib)
        else:
            runtime_lib.unlink()
    runtime_lib.symlink_to(runtime_lib_source_dir)

    if args.suite == "single":
        prepare_single(runtime_bin)
    else:
        prepare_multi(runtime_bin)

    print(runtime_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
