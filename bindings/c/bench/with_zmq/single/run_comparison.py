#!/usr/bin/env python3
"""Single benchmark comparison runner (libzmq vs zlink) with perf-style result policy."""

from __future__ import annotations

import argparse
import datetime
import os
import platform
import statistics
import subprocess
import sys
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Tuple

IS_WINDOWS = os.name == "nt"
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""

SCRIPT_DIR = os.path.abspath(os.path.dirname(__file__))
BENCH_ZMQ_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
ROOT_DIR = os.path.abspath(os.path.join(BENCH_ZMQ_ROOT, "..", "..", "..", ".."))
BUILD_CONFIG_DIRS = ("Release", "Debug", "RelWithDebInfo", "MinSizeRel")

DEFAULT_RESULTS_DIR = os.path.join(BENCH_ZMQ_ROOT, "results")
DEFAULT_MAX_RESULT_FILES = 100
DEFAULT_MSG_SIZES = [64, 256, 1024, 65536, 131072, 262144]
DEFAULT_PATTERNS = [
    "PAIR",
    "PUBSUB",
    "DEALER_DEALER",
    "DEALER_ROUTER",
    "ROUTER_ROUTER",
]

PATTERN_COMPARISONS: List[Tuple[str, str, str]] = [
    ("comp_std_zmq_pair", "comp_zlink_pair", "PAIR"),
    ("comp_std_zmq_pubsub", "comp_zlink_pubsub", "PUBSUB"),
    ("comp_std_zmq_dealer_dealer", "comp_zlink_dealer_dealer", "DEALER_DEALER"),
    ("comp_std_zmq_dealer_router", "comp_zlink_dealer_router", "DEALER_ROUTER"),
    ("comp_std_zmq_router_router", "comp_zlink_router_router", "ROUTER_ROUTER"),
]

if IS_WINDOWS:
    DEFAULT_TRANSPORTS = ["tcp", "inproc"]
else:
    DEFAULT_TRANSPORTS = ["tcp", "ipc", "inproc"]


@dataclass
class RunOutcome:
    parsed: Dict[str, float]
    timed_out: bool
    returncode: int
    error: str


class TeeStream:
    def __init__(self, *streams):
        self._streams = streams

    def write(self, data: str) -> int:
        for stream in self._streams:
            stream.write(data)
            stream.flush()
        return len(data)

    def flush(self) -> None:
        for stream in self._streams:
            stream.flush()


def platform_arch_tag() -> Tuple[str, str]:
    sys_name = platform.system().lower()
    if "darwin" in sys_name:
        platform_tag = "macos"
    elif "windows" in sys_name:
        platform_tag = "windows"
    else:
        platform_tag = "linux"

    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        arch_tag = "x64"
    elif machine in ("aarch64", "arm64"):
        arch_tag = "arm64"
    else:
        arch_tag = machine
    return platform_tag, arch_tag


def detect_platform_tag() -> str:
    return platform_arch_tag()[0]


def build_result_filename(tag: str) -> str:
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    name = f"perf_{detect_platform_tag()}_{ts}"
    clean_tag = (tag or "").strip()
    if clean_tag:
        name += f"_{clean_tag}"
    return name + ".txt"


def single_result_dir(results_root: str) -> str:
    return os.path.join(results_root, "single", "report")


def enforce_file_retention(dir_path: str, max_files: int = DEFAULT_MAX_RESULT_FILES) -> None:
    if max_files < 1 or not os.path.isdir(dir_path):
        return

    files: List[str] = []
    for name in os.listdir(dir_path):
        path = os.path.join(dir_path, name)
        if os.path.isfile(path):
            files.append(name)

    if len(files) <= max_files:
        return

    files.sort()
    remove_count = len(files) - max_files
    for name in files[:remove_count]:
        try:
            os.remove(os.path.join(dir_path, name))
        except OSError:
            pass


def resolve_linux_paths() -> Tuple[str, str, str]:
    platform_tag, arch_tag = platform_arch_tag()
    if platform_tag == "windows":
        platform_tag = "linux"

    possible_paths = [
        os.path.join(ROOT_DIR, "core", "build", f"{platform_tag}-{arch_tag}", "bin"),
        os.path.join(
            ROOT_DIR,
            "core",
            "build",
            f"{platform_tag}-{arch_tag}",
            "bin",
            "Release",
        ),
        os.path.join(ROOT_DIR, "core", "build", "bin"),
    ]
    build_dir = next((p for p in possible_paths if os.path.exists(p)), possible_paths[0])

    linux_dist_dir = os.path.join(
        BENCH_ZMQ_ROOT,
        "libzmq",
        "libzmq_dist",
        "linux-x64",
        "lib",
    )
    default_dist_dir = os.path.join(BENCH_ZMQ_ROOT, "libzmq", "libzmq_dist", "lib")
    libzmq_lib_dir = os.path.abspath(
        linux_dist_dir if os.path.exists(linux_dist_dir) else default_dist_dir
    )
    env_libzmq_dir = os.environ.get("BENCH_LIBZMQ_LIB_DIR")
    if env_libzmq_dir:
        libzmq_lib_dir = os.path.abspath(env_libzmq_dir)

    build_root = build_dir
    base = os.path.basename(build_root)
    if base in BUILD_CONFIG_DIRS:
        bin_root = os.path.dirname(build_root)
        if os.path.basename(bin_root) == "bin":
            build_root = os.path.dirname(bin_root)
    elif base == "bin":
        build_root = os.path.dirname(build_root)
    zlink_lib_dir = os.path.abspath(os.path.join(build_root, "lib"))
    return build_dir, libzmq_lib_dir, zlink_lib_dir


def runtime_bin_dir_from_cmake_dir(cmake_build_dir: str) -> str:
    bin_dir = os.path.join(cmake_build_dir, "bin")
    if IS_WINDOWS:
        return os.path.join(bin_dir, "Release")
    return bin_dir


def normalize_build_dir(path: str) -> str:
    if not path:
        return path

    abs_path = os.path.abspath(path)
    if not os.path.isdir(abs_path):
        base = os.path.basename(abs_path)
        if base in BUILD_CONFIG_DIRS:
            return abs_path
        if base == "bin":
            if IS_WINDOWS:
                return os.path.join(abs_path, "Release")
            return abs_path
        if os.path.basename(os.path.dirname(abs_path)) == "bin":
            return abs_path
        return runtime_bin_dir_from_cmake_dir(abs_path)

    base = os.path.basename(abs_path)
    if base in BUILD_CONFIG_DIRS:
        return abs_path

    if base == "bin":
        if IS_WINDOWS:
            return os.path.join(abs_path, "Release")
        return abs_path

    bin_dir = os.path.join(abs_path, "bin")
    if os.path.isdir(bin_dir):
        if IS_WINDOWS:
            return os.path.join(bin_dir, "Release")
        return bin_dir

    if os.path.isfile(os.path.join(abs_path, "CMakeCache.txt")):
        return runtime_bin_dir_from_cmake_dir(abs_path)

    return abs_path


def derive_zlink_lib_dir(build_dir: str) -> str:
    build_root = build_dir
    base = os.path.basename(build_root)
    if base in BUILD_CONFIG_DIRS:
        bin_root = os.path.dirname(build_root)
        if os.path.basename(bin_root) == "bin":
            build_root = os.path.dirname(bin_root)
    elif base == "bin":
        build_root = os.path.dirname(build_root)
    return os.path.abspath(os.path.join(build_root, "lib"))


def has_cmake_cache(path: str) -> bool:
    return os.path.isfile(os.path.join(path, "CMakeCache.txt"))


def derive_cmake_build_dir(runtime_build_dir: str) -> str:
    if not runtime_build_dir:
        return ""

    abs_path = os.path.abspath(runtime_build_dir)
    if has_cmake_cache(abs_path):
        return abs_path

    base = os.path.basename(abs_path)
    if base in BUILD_CONFIG_DIRS:
        bin_root = os.path.dirname(abs_path)
        if os.path.basename(bin_root) == "bin":
            return os.path.dirname(bin_root)
        return os.path.dirname(abs_path)
    if base == "bin":
        return os.path.dirname(abs_path)

    parent = os.path.dirname(abs_path)
    if os.path.basename(parent) == "bin":
        return os.path.dirname(parent)
    return abs_path


def expected_runtime_binaries(selected_comparisons: Sequence[Tuple[str, str, str]]) -> List[str]:
    names: List[str] = []
    for std_bin, zlk_bin, _ in selected_comparisons:
        names.append(std_bin)
        names.append(zlk_bin)
    return names


def collect_missing_binaries(runtime_bin_dir: str, names: Sequence[str]) -> List[str]:
    missing: List[str] = []
    for name in names:
        bin_path = os.path.join(runtime_bin_dir, name + EXE_SUFFIX)
        if not os.path.exists(bin_path):
            missing.append(name)
    return sorted(set(missing))


def collect_single_build_targets(comparisons: Sequence[Tuple[str, str, str]]) -> List[str]:
    targets: List[str] = []
    for std_bin, zlk_bin, _ in comparisons:
        if std_bin not in targets:
            targets.append(std_bin)
        if zlk_bin not in targets:
            targets.append(zlk_bin)
    return targets


def run_cmake_build(cmake_build_dir: str, targets: Sequence[str]) -> int:
    cmd = ["cmake", "--build", cmake_build_dir]
    if IS_WINDOWS:
        cmd.extend(["--config", "Release"])
    if targets:
        cmd.append("--target")
        cmd.extend(targets)

    print(f"  > Auto-building missing benchmark binaries in {cmake_build_dir}")
    print(f"  > Build command: {' '.join(cmd)}")
    try:
        return subprocess.run(cmd).returncode
    except FileNotFoundError:
        print("Error: cmake not found in PATH", file=sys.stderr)
        return 127


def detect_cmake_source_dir(cmake_build_dir: str) -> str:
    cache_path = os.path.join(cmake_build_dir, "CMakeCache.txt")
    if os.path.isfile(cache_path):
        try:
            with open(cache_path, "r", encoding="utf-8") as cache_file:
                for line in cache_file:
                    if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
                        source_dir = line.split("=", 1)[1].strip()
                        if source_dir:
                            return source_dir
        except OSError:
            pass

    root_cmake = os.path.join(ROOT_DIR, "CMakeLists.txt")
    if os.path.isfile(root_cmake):
        return ROOT_DIR
    return os.path.join(ROOT_DIR, "core")


def run_cmake_configure(cmake_build_dir: str) -> int:
    source_dir = detect_cmake_source_dir(cmake_build_dir)
    cmd = [
        "cmake",
        "-S",
        source_dir,
        "-B",
        cmake_build_dir,
        "-DBUILD_SHARED=ON",
        "-DBUILD_BENCHMARKS=ON",
        "-DZLINK_BUILD_BENCH_ZMQ=ON",
        "-DZLINK_BUILD_WITH_ZMQ_ZLINK_BENCHES=ON",
        "-DZLINK_BUILD_BENCH_ZLINK=ON",
        "-DZLINK_BUILD_BENCH_BEAST=OFF",
    ]

    print(f"  > Configuring benchmark build in {cmake_build_dir}")
    print(f"  > Configure command: {' '.join(cmd)}")
    try:
        return subprocess.run(cmd).returncode
    except FileNotFoundError:
        print("Error: cmake not found in PATH", file=sys.stderr)
        return 127


def parse_env_list(name: str, cast_fn):
    val = os.environ.get(name)
    if not val:
        return None

    out: List = []
    for part in val.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            out.append(cast_fn(part))
        except ValueError:
            continue
    return out or None


def parse_env_list_any(names: Sequence[str], cast_fn):
    for name in names:
        parsed = parse_env_list(name, cast_fn)
        if parsed:
            return parsed
    return None


def parse_env_int_any(names: Sequence[str], default: int) -> int:
    for name in names:
        val = os.environ.get(name)
        if not val:
            continue
        try:
            return int(val)
        except ValueError:
            continue
    return default


def parse_list_value(raw: str, cast_fn):
    out: List = []
    for part in str(raw).split(","):
        part = part.strip()
        if not part:
            continue
        try:
            out.append(cast_fn(part))
        except ValueError:
            return None
    return out or None


def parse_pattern_arg(raw: str) -> List[str]:
    token = (raw or "ALL").strip().upper()
    if not token:
        token = "ALL"

    aliases = {
        "PAIR": "PAIR",
        "PUBSUB": "PUBSUB",
        "DEALER_DEALER": "DEALER_DEALER",
        "DEALER_ROUTER": "DEALER_ROUTER",
        "ROUTER_ROUTER": "ROUTER_ROUTER",
    }

    if token == "ALL":
        return list(DEFAULT_PATTERNS)

    out: List[str] = []
    for part in token.split(","):
        p = part.strip().upper()
        if p.startswith("SINGLE_"):
            p = p[len("SINGLE_") :]
        mapped = aliases.get(p)
        if not mapped:
            raise ValueError(
                "unsupported patterns: " + p + ". Supported: " + ", ".join(DEFAULT_PATTERNS)
            )
        if mapped not in out:
            out.append(mapped)

    if not out:
        raise ValueError("--pattern requires at least one value")
    return out


def validate_transports(transports: Sequence[str]) -> List[str]:
    unknown = [t for t in transports if t not in DEFAULT_TRANSPORTS]
    if unknown:
        raise ValueError(
            "unsupported transports: "
            + ", ".join(sorted(set(unknown)))
            + ". Supported: "
            + ", ".join(DEFAULT_TRANSPORTS)
        )

    ordered: List[str] = []
    for t in DEFAULT_TRANSPORTS:
        if t in transports:
            ordered.append(t)
    for t in transports:
        if t not in ordered:
            ordered.append(t)
    return ordered


def format_throughput(msgs_per_sec: float) -> str:
    return f"{msgs_per_sec/1e3:6.2f} Kmsg/s"


def metric_or_none(metric_map: Dict[str, float], key: str):
    if key not in metric_map:
        return None
    try:
        return float(metric_map[key])
    except Exception:
        return None


def format_queue_metric(value: Optional[float]) -> str:
    if value is None:
        return "N/A"
    return f"{value:8.2f}"


def build_transport_report_header_lines(transport: str) -> List[str]:
    size_w = 6
    metric_w = 10
    val_w = 16
    diff_w = 9
    return [
        f"### Transport: {transport}",
        (
            f"| {'Size':<{size_w}} | {'Metric':<{metric_w}} | {'Standard libzmq':>{val_w}} | "
            f"{'zlink':>{val_w}} | {'Diff (%)':>{diff_w}} |"
        ),
        (
            f"|{'-' * (size_w + 2)}|{'-' * (metric_w + 2)}|{'-' * (val_w + 2)}|"
            f"{'-' * (val_w + 2)}|{'-' * (diff_w + 2)}|"
        ),
    ]


def build_size_report_lines(
    std_data: Dict[str, float],
    zlk_data: Dict[str, float],
    transport: str,
    size: int,
) -> List[str]:
    size_w = 6
    metric_w = 10
    val_w = 16
    diff_w = 9

    k_t = f"{transport}|{size}|throughput"
    k_l = f"{transport}|{size}|latency"

    std_t = metric_or_none(std_data, k_t)
    std_l = metric_or_none(std_data, k_l)
    zlk_t = metric_or_none(zlk_data, k_t)
    zlk_l = metric_or_none(zlk_data, k_l)

    std_t_s = format_throughput(std_t) if std_t is not None else "N/A"
    std_l_s = f"{std_l/1e3:8.2f} ms" if std_l is not None else "N/A"
    zlk_t_s = format_throughput(zlk_t) if zlk_t is not None else "N/A"
    zlk_l_s = f"{zlk_l/1e3:8.2f} ms" if zlk_l is not None else "N/A"

    if std_t is not None and zlk_t is not None and std_t > 0:
        t_diff = (zlk_t - std_t) / std_t * 100.0
        t_diff_s = f"{t_diff:>+7.2f}%"
    else:
        t_diff_s = "N/A"

    if std_l is not None and zlk_l is not None and std_l > 0:
        l_diff = (std_l - zlk_l) / std_l * 100.0
        l_diff_s = f"{l_diff:>+7.2f}%"
    else:
        l_diff_s = "N/A"

    lines = [
        (
            f"| {f'{size}B':<{size_w}} | {'Throughput':<{metric_w}} | {std_t_s:>{val_w}} | "
            f"{zlk_t_s:>{val_w}} | {t_diff_s:>{diff_w}} |"
        ),
        (
            f"| {f'{size}B':<{size_w}} | {'Latency':<{metric_w}} | {std_l_s:>{val_w}} | "
            f"{zlk_l_s:>{val_w}} | {l_diff_s:>{diff_w}} |"
        ),
    ]

    queue_metric_specs = [
        ("snd_pending_max", "SndPending"),
        ("rcv_pending_max", "RcvPending"),
        ("rcv_pending_end", "RcvEnd"),
    ]
    for metric_key, metric_label in queue_metric_specs:
        std_metric = metric_or_none(std_data, f"{transport}|{size}|{metric_key}")
        zlk_metric = metric_or_none(zlk_data, f"{transport}|{size}|{metric_key}")
        if std_metric is None and zlk_metric is None:
            continue
        lines.append(
            (
                f"| {f'{size}B':<{size_w}} | {metric_label:<{metric_w}} | "
                f"{format_queue_metric(std_metric):>{val_w}} | "
                f"{format_queue_metric(zlk_metric):>{val_w}} | {'N/A':>{diff_w}} |"
            )
        )

    return lines


def build_pattern_report_lines(
    std_data: Dict[str, float],
    zlk_data: Dict[str, float],
    transports: Sequence[str],
    msg_sizes: Sequence[int],
) -> List[str]:
    lines: List[str] = []
    for tr in transports:
        lines.extend(build_transport_report_header_lines(tr))
        for sz in msg_sizes:
            lines.extend(build_size_report_lines(std_data, zlk_data, tr, sz))

        lines.append("")

    while lines and lines[-1] == "":
        lines.pop()
    return lines


def get_env_for_lib(lib_name: str, base_env: Dict[str, str], libzmq_lib_dir: str, zlink_lib_dir: str):
    env = base_env.copy()
    if IS_WINDOWS:
        env["PATH"] = f"{libzmq_lib_dir};{env.get('PATH', '')}"
    else:
        if lib_name == "zlink":
            env["LD_LIBRARY_PATH"] = f"{zlink_lib_dir}:{env.get('LD_LIBRARY_PATH', '')}"
        else:
            env["LD_LIBRARY_PATH"] = f"{libzmq_lib_dir}:{env.get('LD_LIBRARY_PATH', '')}"
    return env


def parse_result_line(line: str, transport: str, expected_size: int):
    if not line.startswith("RESULT,"):
        return None

    parts = line.split(",")
    if len(parts) < 7:
        return None

    try:
        line_transport = parts[3]
        line_size = int(parts[4])
        metric = parts[5].strip().lower()
        value = float(parts[6])
    except ValueError:
        return None

    if line_transport != transport or line_size != expected_size:
        return None
    return line_transport, line_size, metric, value


def run_single_test(
    build_dir: str,
    binary_name: str,
    lib_name: str,
    transport: str,
    size: int,
    base_env: Dict[str, str],
    libzmq_lib_dir: str,
    zlink_lib_dir: str,
) -> RunOutcome:
    binary_path = os.path.join(build_dir, binary_name + EXE_SUFFIX)
    env = get_env_for_lib(lib_name, base_env, libzmq_lib_dir, zlink_lib_dir)
    if binary_name == "comp_zlink_pubsub":
        env["PERF_RECV_MODE"] = "recv"
    timeout_sec = max(60, int(os.environ.get("BENCH_SINGLE_TIMEOUT_SECONDS", "120")))

    try:
        if IS_WINDOWS:
            cmd = [binary_path, lib_name, transport, str(size)]
        elif base_env.get("BENCH_TASKSET") == "1":
            cmd = ["taskset", "-c", "1", binary_path, lib_name, transport, str(size)]
        else:
            cmd = [binary_path, lib_name, transport, str(size)]

        result = subprocess.run(
            cmd,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
        )

        parsed: Dict[str, float] = {}
        for line in result.stdout.splitlines():
            parsed_line = parse_result_line(line, transport, size)
            if not parsed_line:
                continue
            line_transport, line_size, metric, value = parsed_line
            parsed[f"{line_transport}|{line_size}|{metric}"] = value

        return RunOutcome(
            parsed=parsed,
            timed_out=False,
            returncode=result.returncode,
            error="",
        )
    except subprocess.TimeoutExpired:
        return RunOutcome(parsed={}, timed_out=True, returncode=-1, error="timeout")
    except Exception as exc:
        return RunOutcome(
            parsed={},
            timed_out=False,
            returncode=-1,
            error=f"exception_{type(exc).__name__}",
        )


def collect_data(
    build_dir: str,
    binary_name: str,
    lib_name: str,
    pattern_name: str,
    num_runs: int,
    transports: Sequence[str],
    msg_sizes: Sequence[int],
    base_env: Dict[str, str],
    libzmq_lib_dir: str,
    zlink_lib_dir: str,
    announce: bool = True,
    on_size_complete: Optional[Callable[[str, int, Dict[str, float]], None]] = None,
    show_progress: bool = True,
) -> Tuple[Dict[str, float], List[Tuple[str, str, str, int, str]]]:
    if announce:
        print(f"  > Benchmarking {lib_name} for {pattern_name}...")

    final_stats: Dict[str, float] = {}
    failures: List[Tuple[str, str, str, int, str]] = []

    for tr in transports:
        for sz in msg_sizes:
            if show_progress:
                print(f"    Testing {tr} | {sz}B: ", end="", flush=True)
            metric_buckets: Dict[str, List[float]] = {}
            failed_runs = 0
            expected_throughput = f"{tr}|{sz}|throughput"
            expected_latency = f"{tr}|{sz}|latency"

            for i in range(num_runs):
                if show_progress:
                    print(f"{i+1} ", end="", flush=True)
                run_outcome = run_single_test(
                    build_dir,
                    binary_name,
                    lib_name,
                    tr,
                    sz,
                    base_env,
                    libzmq_lib_dir,
                    zlink_lib_dir,
                )

                if run_outcome.timed_out:
                    failed_runs += 1
                    failures.append((pattern_name, lib_name, tr, sz, "timeout"))
                    continue

                if run_outcome.error:
                    failed_runs += 1
                    failures.append((pattern_name, lib_name, tr, sz, run_outcome.error))
                    continue

                results = run_outcome.parsed
                if not results:
                    failed_runs += 1
                    reason = (
                        f"no_data_rc_{run_outcome.returncode}"
                        if run_outcome.returncode not in (0, None)
                        else "no_data"
                    )
                    failures.append((pattern_name, lib_name, tr, sz, reason))
                    continue

                missing: List[str] = []
                if expected_throughput not in results:
                    missing.append("throughput")
                if expected_latency not in results:
                    missing.append("latency")
                if missing:
                    failed_runs += 1
                    suffix = (
                        f"_rc_{run_outcome.returncode}"
                        if run_outcome.returncode not in (0, None)
                        else ""
                    )
                    for metric in missing:
                        failures.append(
                            (
                                pattern_name,
                                lib_name,
                                tr,
                                sz,
                                f"missing_{metric}{suffix}",
                            )
                        )
                    continue

                for key, value in results.items():
                    metric_buckets.setdefault(key, []).append(value)

            for key, values in metric_buckets.items():
                if values:
                    final_stats[key] = statistics.median(values)

            if show_progress:
                if failed_runs:
                    print(f"(failures={failed_runs}) ", end="", flush=True)
                print("Done")

            if on_size_complete is not None:
                on_size_complete(tr, sz, final_stats)

    return final_stats, failures


def resolve_patterns_from_args(args: argparse.Namespace) -> List[str]:
    pattern_token = args.pattern
    if args.pattern_opt:
        pattern_token = args.pattern_opt
    if args.patterns_opt:
        pattern_token = args.patterns_opt
    return parse_pattern_arg(pattern_token)


def resolve_transports_from_args(args: argparse.Namespace) -> List[str]:
    cli_transports = args.transports_opt.strip()
    if cli_transports:
        parsed = parse_list_value(cli_transports, str)
        if not parsed:
            raise ValueError(f"invalid transport list '{cli_transports}'")
        return validate_transports(parsed)

    env_transports = parse_env_list_any(
        ["BENCH_TRANSPORTS", "PERF_TRANSPORTS"], str
    )
    if env_transports:
        return validate_transports(env_transports)
    return list(DEFAULT_TRANSPORTS)


def resolve_msg_sizes_from_args(args: argparse.Namespace) -> List[int]:
    cli_sizes = args.msg_sizes_opt.strip()
    if cli_sizes:
        parsed = parse_list_value(cli_sizes, int)
        if not parsed:
            raise ValueError(f"invalid msg size list '{cli_sizes}'")
        sizes = parsed
    else:
        env_sizes = parse_env_list_any(
            ["BENCH_MSG_SIZES", "PERF_MSG_SIZES"], int
        )
        sizes = env_sizes if env_sizes else list(DEFAULT_MSG_SIZES)

    for size in sizes:
        if size < 1:
            raise ValueError("msg sizes must be positive integers")
    return sizes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare single benchmark performance: libzmq vs zlink.",
    )
    parser.add_argument("pattern", nargs="?", default="ALL")
    parser.add_argument("--pattern", dest="pattern_opt", default="")
    parser.add_argument("--patterns", dest="patterns_opt", default="")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--transport", "--transports", dest="transports_opt", default="")
    parser.add_argument("--msg-sizes", dest="msg_sizes_opt", default="")
    parser.add_argument("--build-dir", default="")
    parser.add_argument("--pin-cpu", action="store_true")
    parser.add_argument("--results-dir", default=DEFAULT_RESULTS_DIR)
    parser.add_argument("--results-tag", default="")
    parser.add_argument("--result-file", default="")
    return parser.parse_args()


def build_effective_option_items(
    args: argparse.Namespace,
    patterns: Sequence[str],
    transports: Sequence[str],
    msg_sizes: Sequence[int],
    build_dir: str,
) -> List[Tuple[str, str]]:
    duration = parse_env_int_any(["PERF_SINGLE_DURATION_SECONDS"], 5)
    latency_duration = parse_env_int_any(["PERF_SINGLE_LATENCY_SECONDS"], duration)
    timeout_sec = max(60, parse_env_int_any(["BENCH_SINGLE_TIMEOUT_SECONDS"], 120))
    sndtimeo = parse_env_int_any(["PERF_SINGLE_SNDTIMEO_MS"], 200)
    recvtimeo = parse_env_int_any(
        ["PERF_SINGLE_RCVTIMEO_MS", "PERF_SINGLE_PUBSUB_RCVTIMEO_MS"], 200
    )
    hwm = parse_env_int_any(["PERF_SINGLE_HWM"], 1000)
    sndhwm = parse_env_int_any(["PERF_SINGLE_SNDHWM"], hwm)
    rcvhwm = parse_env_int_any(["PERF_SINGLE_RCVHWM"], hwm)
    io_threads = parse_env_int_any(["PERF_IO_THREADS", "BENCH_IO_THREADS"], 1)
    xpub_nodrop = parse_env_int_any(["PERF_SINGLE_PUBSUB_XPUB_NODROP"], 1)

    items: List[Tuple[str, str]] = [
        ("runs", str(args.runs)),
        ("duration_seconds", str(duration)),
        ("latency_seconds", str(latency_duration)),
        ("timeout_seconds", str(timeout_sec)),
        ("sndtimeo_ms", str(sndtimeo)),
        ("recvtimeo_ms", str(recvtimeo)),
        ("hwm", str(hwm)),
        ("sndhwm", str(sndhwm)),
        ("rcvhwm", str(rcvhwm)),
        ("io_threads", str(io_threads)),
        ("xpub_nodrop", str(xpub_nodrop)),
        ("patterns", ",".join(patterns)),
        ("transports", ",".join(transports)),
        ("msg_sizes", ",".join(str(s) for s in msg_sizes)),
        ("build_dir", build_dir),
    ]
    return items


def main() -> int:
    if IS_WINDOWS:
        default_build_dir = os.path.join(
            ROOT_DIR,
            "core",
            "build",
            "windows-x64",
            "bin",
            "Release",
        )
        libzmq_lib_dir = os.path.join(
            BENCH_ZMQ_ROOT,
            "libzmq",
            "libzmq_dist",
            "windows-x64",
            "bin",
        )
        env_libzmq_dir = os.environ.get("BENCH_LIBZMQ_BIN_DIR")
        if env_libzmq_dir:
            libzmq_lib_dir = os.path.abspath(env_libzmq_dir)
    else:
        default_build_dir, libzmq_lib_dir, _ = resolve_linux_paths()

    args = parse_args()

    if args.runs < 1:
        print("Error: --runs must be >= 1", file=sys.stderr)
        return 1

    try:
        patterns = resolve_patterns_from_args(args)
        transports = resolve_transports_from_args(args)
        msg_sizes = resolve_msg_sizes_from_args(args)
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    if args.build_dir:
        build_dir = normalize_build_dir(args.build_dir)
    else:
        build_dir = normalize_build_dir(default_build_dir)

    zlink_lib_dir = derive_zlink_lib_dir(build_dir)
    base_env = os.environ.copy()
    if args.pin_cpu:
        base_env["BENCH_TASKSET"] = "1"

    requested = set(patterns)
    comparisons = [c for c in PATTERN_COMPARISONS if c[2] in requested]
    if len(comparisons) != len(requested):
        supported = ", ".join(DEFAULT_PATTERNS)
        print(
            f"Error: unsupported pattern in {patterns}. Supported: {supported}",
            file=sys.stderr,
        )
        return 1

    expected_bins = expected_runtime_binaries(comparisons)
    missing = collect_missing_binaries(build_dir, expected_bins)
    no_autobuild = (
        os.environ.get("BENCH_NO_AUTOBUILD") == "1"
        or os.environ.get("PERF_NO_AUTOBUILD") == "1"
    )

    if missing:
        if no_autobuild:
            print(
                f"Error: missing benchmark binaries in {build_dir} and auto-build is disabled.",
                file=sys.stderr,
            )
            for name in missing:
                print(f"  - {name}{EXE_SUFFIX}", file=sys.stderr)
            return 2

        cmake_build_dir = derive_cmake_build_dir(build_dir)
        if not cmake_build_dir:
            print(
                f"Error: missing benchmark binaries in {build_dir} and failed to derive CMake build dir.",
                file=sys.stderr,
            )
            print(
                "Hint: pass --build-dir as a CMake build root or its bin(/Release) directory.",
                file=sys.stderr,
            )
            for name in missing:
                print(f"  - {name}{EXE_SUFFIX}", file=sys.stderr)
            return 2

        configure_rc = run_cmake_configure(cmake_build_dir)
        if configure_rc != 0:
            print(
                f"Error: auto-configure failed with exit code {configure_rc}",
                file=sys.stderr,
            )
            return configure_rc

        build_dir = normalize_build_dir(runtime_bin_dir_from_cmake_dir(cmake_build_dir))
        zlink_lib_dir = derive_zlink_lib_dir(build_dir)
        build_targets = collect_single_build_targets(comparisons)
        build_rc = run_cmake_build(cmake_build_dir, build_targets)
        if build_rc != 0:
            print(f"Error: auto-build failed with exit code {build_rc}", file=sys.stderr)
            return build_rc

        missing = collect_missing_binaries(build_dir, expected_bins)

    if missing:
        print(f"Error: missing benchmark binaries in {build_dir}:", file=sys.stderr)
        for name in missing:
            print(f"  - {name}{EXE_SUFFIX}", file=sys.stderr)
        return 2

    results_root = os.path.abspath(args.results_dir)
    result_dir = single_result_dir(results_root)

    result_file = args.result_file
    if not result_file:
        result_file = os.path.join(result_dir, build_result_filename(args.results_tag))

    result_parent = os.path.dirname(result_file)
    if result_parent:
        os.makedirs(result_parent, exist_ok=True)

    result_log_fh = open(result_file, "w", encoding="utf-8", buffering=1)
    orig_stdout = sys.stdout
    orig_stderr = sys.stderr
    sys.stdout = TeeStream(orig_stdout, result_log_fh)
    sys.stderr = TeeStream(orig_stderr, result_log_fh)

    all_failures: List[Tuple[str, str, str, int, str]] = []
    any_failure = False
    print_final_report = (
        os.environ.get("BENCH_SINGLE_PRINT_FINAL_REPORT", "0") == "1"
    )

    try:
        option_items = build_effective_option_items(
            args, patterns, transports, msg_sizes, build_dir
        )
        print("\n## Effective Options (single)")
        for key, value in option_items:
            print(f"- {key}: {value}")

        for std_bin, zlk_bin, pattern_name in comparisons:
            print(f"\n## PATTERN: {pattern_name}")

            for tr in transports:
                std_data: Dict[str, float] = {}
                zlk_data: Dict[str, float] = {}
                std_fail: List[Tuple[str, str, str, int, str]] = []
                zlk_fail: List[Tuple[str, str, str, int, str]] = []
                printed_live_header = False
                live_rows = 0
                announced_std = False
                announced_zlk = False

                for sz in msg_sizes:
                    std_partial, std_partial_fail = collect_data(
                        build_dir,
                        std_bin,
                        "libzmq",
                        pattern_name,
                        args.runs,
                        [tr],
                        [sz],
                        base_env,
                        libzmq_lib_dir,
                        zlink_lib_dir,
                        announce=not announced_std,
                        show_progress=False,
                    )
                    announced_std = True
                    std_data.update(std_partial)
                    std_fail.extend(std_partial_fail)

                    zlk_partial, zlk_partial_fail = collect_data(
                        build_dir,
                        zlk_bin,
                        "zlink",
                        pattern_name,
                        args.runs,
                        [tr],
                        [sz],
                        base_env,
                        libzmq_lib_dir,
                        zlink_lib_dir,
                        announce=not announced_zlk,
                        show_progress=False,
                    )
                    announced_zlk = True
                    zlk_data.update(zlk_partial)
                    zlk_fail.extend(zlk_partial_fail)

                    if not printed_live_header:
                        for line in build_transport_report_header_lines(tr):
                            print(line)
                        printed_live_header = True
                    for line in build_size_report_lines(std_data, zlk_data, tr, sz):
                        print(line)
                    live_rows += 1

                if print_final_report or live_rows == 0:
                    transport_block = build_pattern_report_lines(
                        std_data, zlk_data, [tr], msg_sizes
                    )
                    for line in transport_block:
                        print(line)

                all_failures.extend(std_fail)
                all_failures.extend(zlk_fail)
                if std_fail or zlk_fail:
                    any_failure = True

        if all_failures:
            print("\n## Failures")
            for pattern, lib_name, tr, sz, reason in all_failures:
                print(f"- {pattern} {lib_name} {tr} {sz}B: {reason}")

        enforce_file_retention(os.path.dirname(result_file))
        print(f"\nSaved result file: {result_file}")
    finally:
        sys.stdout = orig_stdout
        sys.stderr = orig_stderr
        result_log_fh.close()

    return 1 if any_failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
