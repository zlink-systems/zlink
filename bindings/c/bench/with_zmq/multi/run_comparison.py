#!/usr/bin/env python3
import json
import os
import platform
import selectors
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone

IS_WINDOWS = os.name == "nt"
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""
SCRIPT_DIR = os.path.abspath(os.path.dirname(__file__))
ROOT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", "..", ".."))
BUILD_CONFIG_DIRS = ("Release", "Debug", "RelWithDebInfo", "MinSizeRel")
BENCH_ZMQ_ROOT = os.path.join(ROOT_DIR, "bindings", "c", "bench", "with_zmq")
if not os.path.isdir(BENCH_ZMQ_ROOT):
    BENCH_ZMQ_ROOT = os.path.join(ROOT_DIR, "bindings", "c", "bench", "benchwithzmq")


def platform_arch_tag():
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


def resolve_linux_paths():
    sys_name = platform.system().lower()
    if "darwin" in sys_name:
        platform_tag = "macos"
    else:
        platform_tag = "linux"
    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        arch_tag = "x64"
    elif machine in ("aarch64", "arm64"):
        arch_tag = "arm64"
    else:
        arch_tag = machine

    possible_paths = [
        os.path.join(ROOT_DIR, "core", "build", f"{platform_tag}-{arch_tag}", "bin"),
        os.path.join(
            ROOT_DIR, "core", "build", f"{platform_tag}-{arch_tag}", "bin", "Release"
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
    default_dist_dir = os.path.join(
        BENCH_ZMQ_ROOT, "libzmq", "libzmq_dist", "lib"
    )
    libzmq_lib_dir = os.path.abspath(linux_dist_dir if os.path.exists(linux_dist_dir) else default_dist_dir)
    env_libzmq_dir = os.environ.get("BENCH_LIBZMQ_LIB_DIR")
    if env_libzmq_dir:
        libzmq_lib_dir = os.path.abspath(env_libzmq_dir)

    build_root = build_dir
    base = os.path.basename(build_root)
    if base in ("Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
        bin_root = os.path.dirname(build_root)
        if os.path.basename(bin_root) == "bin":
            build_root = os.path.dirname(bin_root)
    elif base == "bin":
        build_root = os.path.dirname(build_root)
    zlink_lib_dir = os.path.abspath(os.path.join(build_root, "lib"))
    return build_dir, libzmq_lib_dir, zlink_lib_dir


def runtime_bin_dir_from_cmake_dir(cmake_build_dir):
    bin_dir = os.path.join(cmake_build_dir, "bin")
    if IS_WINDOWS:
        return os.path.join(bin_dir, "Release")
    return bin_dir


def normalize_build_dir(path):
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
            release_dir = os.path.join(abs_path, "Release")
            return release_dir
        return abs_path

    bin_dir = os.path.join(abs_path, "bin")
    if os.path.isdir(bin_dir):
        if IS_WINDOWS:
            release_dir = os.path.join(bin_dir, "Release")
            return release_dir
        return bin_dir

    if os.path.isfile(os.path.join(abs_path, "CMakeCache.txt")):
        return runtime_bin_dir_from_cmake_dir(abs_path)

    return abs_path


def derive_zlink_lib_dir(build_dir):
    build_root = build_dir
    base = os.path.basename(build_root)
    if base in BUILD_CONFIG_DIRS:
        bin_root = os.path.dirname(build_root)
        if os.path.basename(bin_root) == "bin":
            build_root = os.path.dirname(bin_root)
    elif base == "bin":
        build_root = os.path.dirname(build_root)
    return os.path.abspath(os.path.join(build_root, "lib"))


def has_cmake_cache(path):
    return os.path.isfile(os.path.join(path, "CMakeCache.txt"))


def derive_cmake_build_dir(runtime_build_dir):
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
    elif base == "bin":
        return os.path.dirname(abs_path)

    parent = os.path.dirname(abs_path)
    if os.path.basename(parent) == "bin":
        return os.path.dirname(parent)
    return abs_path


if IS_WINDOWS:
    BUILD_DIR = os.path.join(ROOT_DIR, "core", "build", "windows-x64", "bin", "Release")
    LIBZMQ_LIB_DIR = os.path.join(
        BENCH_ZMQ_ROOT,
        "libzmq",
        "libzmq_dist",
        "windows-x64",
        "bin",
    )
    env_libzmq_dir = os.environ.get("BENCH_LIBZMQ_BIN_DIR")
    if env_libzmq_dir:
        LIBZMQ_LIB_DIR = os.path.abspath(env_libzmq_dir)
    ZLINK_LIB_DIR = os.path.join(ROOT_DIR, "core", "build", "windows-x64", "bin", "Release")
else:
    BUILD_DIR, LIBZMQ_LIB_DIR, ZLINK_LIB_DIR = resolve_linux_paths()

DEFAULT_NUM_RUNS = 3
PATTERN_SEPARATOR = "==============================================================================="
SPLIT_SERVER_CLIENT_PATTERNS = {
    "MULTI_DEALER_DEALER",
    "MULTI_DEALER_ROUTER",
    "MULTI_ROUTER_ROUTER",
    "MULTI_PUBSUB",
    "MULTI_STREAM",
}
SPLIT_BINARIES = {
    "zlink": {
    "MULTI_DEALER_DEALER": (
        "comp_zlink_multi_dealer_dealer_server",
        "comp_zlink_multi_dealer_dealer_client",
    ),
    "MULTI_DEALER_ROUTER": (
        "comp_zlink_multi_dealer_router_server",
        "comp_zlink_multi_dealer_router_client",
    ),
    "MULTI_ROUTER_ROUTER": (
        "comp_zlink_multi_router_router_server",
        "comp_zlink_multi_router_router_client",
    ),
    "MULTI_PUBSUB": (
        "comp_zlink_multi_pubsub_server",
        "comp_zlink_multi_pubsub_client",
    ),
    "MULTI_STREAM": (
        "comp_zlink_multi_stream_server",
        "comp_zlink_multi_stream",
    ),
    },
    "std_zmq": {
    "MULTI_DEALER_DEALER": (
        "comp_std_zmq_multi_dealer_dealer_server",
        "comp_std_zmq_multi_dealer_dealer_client",
    ),
    "MULTI_DEALER_ROUTER": (
        "comp_std_zmq_multi_dealer_router_server",
        "comp_std_zmq_multi_dealer_router_client",
    ),
    "MULTI_ROUTER_ROUTER": (
        "comp_std_zmq_multi_router_router_server",
        "comp_std_zmq_multi_router_router_client",
    ),
    "MULTI_PUBSUB": (
        "comp_std_zmq_multi_pubsub_server",
        "comp_std_zmq_multi_pubsub_client",
    ),
    "MULTI_STREAM": (
        "comp_std_zmq_multi_stream_server",
        "comp_std_zmq_multi_stream_client",
    ),
    },
}
_platform_tag, _arch_tag = platform_arch_tag()
DEFAULT_STD_CACHE_FILE = os.path.join(
    SCRIPT_DIR, f"std_zmq_multi_cache_{_platform_tag}-{_arch_tag}.json"
)


def canonical_lib_name(lib_name):
    if lib_name in ("libzmq", "std_zmq"):
        return "std_zmq"
    return lib_name


def parse_env_list(name, cast_fn):
    val = os.environ.get(name)
    if not val:
        return None
    out = []
    for part in val.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            out.append(cast_fn(part))
        except ValueError:
            continue
    return out or None


_env_transports = parse_env_list("BENCH_TRANSPORTS", str)
TRANSPORTS = _env_transports if _env_transports else ["tcp"]

_env_sizes = parse_env_list("BENCH_MSG_SIZES", int)
MSG_SIZES = _env_sizes if _env_sizes else [64, 256, 1024, 65536, 131072, 262144]

FAIL_FAST = os.environ.get("BENCH_FAIL_FAST", "0") == "1"
SHOW_PREP = (
    os.environ.get("BENCH_STREAM_SHOW_PREP", "0") == "1"
    or os.environ.get("BENCH_DEBUG", "0") == "1"
)
base_env = os.environ.copy()


def transport_supported_for_pattern(pattern_name, transport):
    if pattern_name == "MULTI_STREAM":
        return transport == "tcp"
    return transport in ("tcp", "ipc")


def parse_nonnegative_int_env(name, default_value):
    raw = os.environ.get(name)
    if raw is None:
        return default_value

    try:
        return max(0, int(raw))
    except ValueError:
        return default_value


DEFAULT_RUN_COOLDOWN_MS = parse_nonnegative_int_env(
    "BENCH_MULTI_RUN_COOLDOWN_MS", 3000
)
TRANSPORT_TRANSITION_MS = parse_nonnegative_int_env(
    "BENCH_TRANSPORT_TRANSITION_MS",
    parse_nonnegative_int_env("PERF_TRANSPORT_TRANSITION_MS", 3000),
)
PATTERN_TRANSITION_MS = parse_nonnegative_int_env(
    "BENCH_MULTI_PATTERN_TRANSITION_MS",
    parse_nonnegative_int_env("PERF_PATTERN_TRANSITION_MS", 3000),
)
SERVER_READY_TIMEOUT_MS = parse_nonnegative_int_env(
    "BENCH_MULTI_SERVER_READY_TIMEOUT_MS",
    parse_nonnegative_int_env(
        "BENCH_SERVER_READY_TIMEOUT_MS",
        parse_nonnegative_int_env("PERF_SERVER_READY_TIMEOUT_MS", 10000),
    ),
)
SERVER_SHUTDOWN_TIMEOUT_MS = parse_nonnegative_int_env(
    "BENCH_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS",
    parse_nonnegative_int_env(
        "BENCH_SERVER_SHUTDOWN_TIMEOUT_MS",
        parse_nonnegative_int_env("PERF_SERVER_SHUTDOWN_TIMEOUT_MS", 5000),
    ),
)


def select_comparisons(comparisons, pattern_req):
    selected = []
    for item in comparisons:
        if pattern_req != "ALL" and item[2] != pattern_req:
            continue
        selected.append(item)
    return selected


def required_split_binaries(lib_name, pattern_name):
    pair = SPLIT_BINARIES.get(canonical_lib_name(lib_name), {}).get(pattern_name)
    if not pair:
        return []
    server_bin, client_bin = pair
    return [server_bin, client_bin]


def expected_runtime_binaries(selected_comparisons, include_std):
    names = []
    for std_bin, zlk_bin, pattern_name in selected_comparisons:
        names.append(zlk_bin)
        names.extend(required_split_binaries("zlink", pattern_name))
        if include_std:
            names.append(std_bin)
            names.extend(required_split_binaries("std_zmq", pattern_name))
    return sorted(set(names))


def collect_missing_binaries(runtime_bin_dir, names):
    missing = []
    for name in names:
        bin_path = os.path.join(runtime_bin_dir, name + EXE_SUFFIX)
        if not os.path.exists(bin_path):
            missing.append(name)
    return sorted(set(missing))


def collect_multi_build_targets(comparisons, include_std):
    targets = []
    for std_bin, zlk_bin, pattern_name in comparisons:
        if include_std and std_bin not in targets:
            targets.append(std_bin)
        if zlk_bin not in targets:
            targets.append(zlk_bin)
        for extra_bin in required_split_binaries("zlink", pattern_name):
            if extra_bin not in targets:
                targets.append(extra_bin)
        if include_std:
            for extra_bin in required_split_binaries("std_zmq", pattern_name):
                if extra_bin not in targets:
                    targets.append(extra_bin)
    return targets


def run_cmake_build(cmake_build_dir, targets):
    cmd = ["cmake", "--build", cmake_build_dir]
    if IS_WINDOWS:
        cmd.extend(["--config", "Release"])
    if targets:
        cmd.append("--target")
        cmd.extend(targets)

    print(
        f"  > Auto-building missing benchmark binaries in {cmake_build_dir}",
        flush=True,
    )
    print(f"  > Build command: {' '.join(cmd)}", flush=True)
    try:
        return subprocess.run(cmd).returncode
    except FileNotFoundError:
        print("Error: cmake not found in PATH", file=sys.stderr)
        return 127


def detect_cmake_source_dir(cmake_build_dir):
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


def run_cmake_configure(cmake_build_dir):
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

    print(
        f"  > Configuring benchmark build in {cmake_build_dir}",
        flush=True,
    )
    print(f"  > Configure command: {' '.join(cmd)}", flush=True)
    try:
        return subprocess.run(cmd).returncode
    except FileNotFoundError:
        print("Error: cmake not found in PATH", file=sys.stderr)
        return 127


def get_env_for_lib(lib_name):
    lib_name = canonical_lib_name(lib_name)
    env = base_env.copy()
    env["BENCH_MSG_SIZES"] = ",".join(str(sz) for sz in MSG_SIZES)
    if IS_WINDOWS:
        env["PATH"] = f"{LIBZMQ_LIB_DIR};{env.get('PATH', '')}"
    else:
        if lib_name == "zlink":
            env["LD_LIBRARY_PATH"] = f"{ZLINK_LIB_DIR}:{env.get('LD_LIBRARY_PATH', '')}"
        else:
            env["LD_LIBRARY_PATH"] = f"{LIBZMQ_LIB_DIR}:{env.get('LD_LIBRARY_PATH', '')}"
    return env


def parse_result_line(line, transport, expected_sizes):
    if not line.startswith("RESULT,"):
        return None
    p = line.split(",")
    if len(p) < 7:
        return None
    try:
        line_transport = p[3]
        line_size = int(p[4])
        metric = p[5].strip().lower()
        value = float(p[6])
    except ValueError:
        return None
    if line_transport != transport or line_size not in expected_sizes:
        return None
    if metric not in ("throughput", "latency"):
        return None
    return line_transport, line_size, metric, value


def parse_prep_line(line, transport, expected_sizes):
    if not line.startswith("PREP,"):
        return None
    p = line.split(",")
    if len(p) < 9:
        return None
    try:
        line_transport = p[3]
        line_size = int(p[4])
        connect_ms = float(p[6])
        ready_ms = float(p[8])
    except ValueError:
        return None
    if line_transport != transport or line_size not in expected_sizes:
        return None
    return line_size, connect_ms, ready_ms


def parse_ready_endpoint_line(line):
    if not line.startswith("READY,"):
        return ""
    parts = line.strip().split(",", 1)
    if len(parts) != 2:
        return ""
    return parts[1].strip()


def parse_client_ready_size(line):
    if not line.startswith("CLIENT_READY,"):
        return None
    parts = line.strip().split(",", 1)
    if len(parts) != 2:
        return None
    try:
        return int(parts[1])
    except ValueError:
        return None


def _stop_server_process(proc, shutdown_timeout_ms):
    if proc is None:
        return
    shutdown_timeout_sec = max(0.0, float(shutdown_timeout_ms) / 1000.0)
    if shutdown_timeout_sec <= 0.0:
        shutdown_timeout_sec = 5.0

    try:
        if proc.stdin is not None:
            proc.stdin.write("STOP\n")
            proc.stdin.flush()
            proc.stdin.close()
    except Exception:
        pass

    try:
        proc.wait(timeout=shutdown_timeout_sec)
    except subprocess.TimeoutExpired:
        try:
            proc.terminate()
            proc.wait(timeout=min(2.0, shutdown_timeout_sec))
        except Exception:
            proc.kill()
            try:
                proc.wait(timeout=1.0)
            except Exception:
                pass


def run_split_server_client_test(
    server_binary_path,
    client_binary_path,
    lib_name,
    transport,
    pattern_name,
    fallback_size,
    expected_sizes,
    timeout_sec,
    server_ready_timeout_ms,
    server_shutdown_timeout_ms,
    warmup_seconds,
    duration_seconds,
    progress_cb=None,
    metric_cb=None,
    env_override=None,
):
    env = dict(env_override) if env_override is not None else get_env_for_lib(lib_name)
    parsed = {}
    expected_metric_count = len(expected_sizes) * 2
    outcome = {
        "parsed": parsed,
        "timed_out": False,
        "returncode": 0,
        "error": "",
    }

    server_cmd = [server_binary_path, lib_name, transport, str(fallback_size), "--server-only"]
    client_cmd = [client_binary_path, lib_name, transport, str(fallback_size)]
    server_proc = None
    selector = None

    try:
        server_proc = subprocess.Popen(
            server_cmd,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        endpoint = ""
        ready_timeout_sec = max(0.001, float(server_ready_timeout_ms) / 1000.0)
        ready_deadline = time.monotonic() + ready_timeout_sec
        selector = selectors.DefaultSelector()
        if server_proc.stdout is not None:
            selector.register(server_proc.stdout, selectors.EVENT_READ)

        while time.monotonic() < ready_deadline:
            if server_proc.poll() is not None:
                break
            events = selector.select(timeout=min(0.5, ready_deadline - time.monotonic()))
            if not events:
                continue
            for key, _ in events:
                line = key.fileobj.readline()
                if not line:
                    continue
                endpoint = parse_ready_endpoint_line(line)
                if endpoint:
                    break
            if endpoint:
                break

        if selector is not None:
            selector.close()
            selector = None

        if not endpoint:
            rc = server_proc.poll()
            if rc is None:
                outcome["timed_out"] = True
                outcome["error"] = "server_ready_timeout"
            else:
                outcome["returncode"] = rc
                outcome["error"] = f"server_start_failed_rc_{rc}"
            return outcome

        cmd = client_cmd + ["--endpoint", endpoint]
        proc = subprocess.Popen(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        deadline = time.monotonic() + timeout_sec
        selector = selectors.DefaultSelector()
        if proc.stdout is not None:
            selector.register(proc.stdout, selectors.EVENT_READ)

        timed_out = False
        process_done = False
        while True:
            if process_done:
                break
            now = time.monotonic()
            if now >= deadline:
                timed_out = True
                break

            events = selector.select(timeout=min(1.0, deadline - now))
            if not events:
                if proc.poll() is not None:
                    break
                continue

            for key, _ in events:
                line = key.fileobj.readline()
                if not line:
                    if proc.poll() is not None:
                        process_done = True
                        break
                    continue
                client_ready_size = parse_client_ready_size(line)
                if client_ready_size is not None:
                    if (
                        client_ready_size in expected_sizes
                        and server_proc is not None
                        and server_proc.stdin is not None
                    ):
                        try:
                            server_proc.stdin.write(f"START,{client_ready_size}\n")
                            server_proc.stdin.flush()
                        except Exception:
                            pass
                    continue
                prep_line = parse_prep_line(line, transport, expected_sizes)
                if prep_line and progress_cb is not None:
                    line_size, connect_ms, ready_ms = prep_line
                    progress_cb(line_size, connect_ms, ready_ms)
                parsed_line = parse_result_line(line, transport, expected_sizes)
                if not parsed_line:
                    continue
                line_transport, line_size, metric, value = parsed_line
                parsed[f"{line_transport}|{line_size}|{metric}"] = value
                if metric_cb is not None:
                    metric_cb(line_size, metric, value)
                if progress_cb is not None and metric == "throughput":
                    progress_cb(line_size)

                if expected_metric_count > 0 and len(parsed) >= expected_metric_count:
                    if proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=2.0)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait()
                    selector.close()
                    outcome["returncode"] = 0
                    return outcome

        if timed_out:
            proc.kill()
            rc = proc.wait()
            selector.close()
            outcome["timed_out"] = True
            outcome["returncode"] = rc
            return outcome

        selector.close()
        if proc.stdout is not None:
            tail = proc.stdout.read()
            if tail:
                for line in tail.splitlines():
                    prep_line = parse_prep_line(line, transport, expected_sizes)
                    if prep_line and progress_cb is not None:
                        line_size, connect_ms, ready_ms = prep_line
                        progress_cb(line_size, connect_ms, ready_ms)
                    parsed_line = parse_result_line(line, transport, expected_sizes)
                    if not parsed_line:
                        continue
                    line_transport, line_size, metric, value = parsed_line
                    parsed[f"{line_transport}|{line_size}|{metric}"] = value
                    if metric_cb is not None:
                        metric_cb(line_size, metric, value)
                    if progress_cb is not None and metric == "throughput":
                        progress_cb(line_size)

        outcome["returncode"] = proc.wait()
        return outcome
    except Exception as exc:
        return {
            "parsed": {},
            "timed_out": False,
            "returncode": -1,
            "error": f"exception_{type(exc).__name__}",
        }
    finally:
        if selector is not None:
            selector.close()
        _stop_server_process(server_proc, server_shutdown_timeout_ms)
        if server_proc is not None and server_proc.stdout is not None:
            try:
                tail = server_proc.stdout.read()
            except Exception:
                tail = ""
            if tail:
                for line in tail.splitlines():
                    prep_line = parse_prep_line(line, transport, expected_sizes)
                    if prep_line and progress_cb is not None:
                        line_size, connect_ms, ready_ms = prep_line
                        progress_cb(line_size, connect_ms, ready_ms)
                    parsed_line = parse_result_line(line, transport, expected_sizes)
                    if not parsed_line:
                        continue
                    line_transport, line_size, metric, value = parsed_line
                    parsed[f"{line_transport}|{line_size}|{metric}"] = value
                    if metric_cb is not None:
                        metric_cb(line_size, metric, value)
                    if progress_cb is not None and metric == "throughput":
                        progress_cb(line_size)


def run_single_test(
    binary_name,
    lib_name,
    transport,
    pattern_name,
    sizes=None,
    progress_cb=None,
    metric_cb=None,
):
    binary_path = os.path.join(BUILD_DIR, binary_name + EXE_SUFFIX)
    lib_name = canonical_lib_name(lib_name)
    env = get_env_for_lib(lib_name)
    env["BENCH_MULTI_PATTERN"] = pattern_name
    size_list = list(sizes) if sizes else list(MSG_SIZES)
    if not size_list:
        size_list = [64]
    size_csv = ",".join(str(sz) for sz in size_list)
    env["BENCH_MSG_SIZES"] = size_csv
    # Keep PERF_* in sync for binaries that read PERF_MSG_SIZES directly.
    env["PERF_MSG_SIZES"] = size_csv

    fallback_size = size_list[0]
    timeout_override = parse_nonnegative_int_env("BENCH_MULTI_TIMEOUT_SECONDS", 0)
    duration_seconds = 5
    warmup_seconds = 3
    try:
        duration_seconds = max(1, int(os.environ.get("BENCH_MULTI_DURATION_SECONDS", "5")))
    except ValueError:
        duration_seconds = 5
    try:
        warmup_seconds = max(0, int(os.environ.get("BENCH_MULTI_WARMUP_SECONDS", "3")))
    except ValueError:
        warmup_seconds = 3
    if timeout_override > 0:
        timeout_sec = timeout_override
    else:
        # Size-isolated execution should fail fast per size instead of waiting
        # for a large whole-pattern timeout window.
        size_timeout_floor = 180 if len(size_list) <= 1 else 300
        timeout_sec = max(
            size_timeout_floor,
            (warmup_seconds + duration_seconds) * max(1, len(size_list)) * 8 + 120,
        )

    expected_sizes = set(size_list)

    try:
        if pattern_name in SPLIT_SERVER_CLIENT_PATTERNS:
            split_server_bin, split_client_bin = SPLIT_BINARIES.get(lib_name, {}).get(
                pattern_name, (None, None)
            )
            if not split_server_bin or not split_client_bin:
                return {
                    "parsed": {},
                    "timed_out": False,
                    "returncode": -1,
                    "error": f"missing_split_mapping_{pattern_name}",
                }
            split_server_path = os.path.join(
                BUILD_DIR, split_server_bin + EXE_SUFFIX
            )
            split_client_path = os.path.join(
                BUILD_DIR, split_client_bin + EXE_SUFFIX
            )
            return run_split_server_client_test(
                split_server_path,
                split_client_path,
                lib_name,
                transport,
                pattern_name,
                fallback_size,
                expected_sizes,
                timeout_sec,
                SERVER_READY_TIMEOUT_MS,
                SERVER_SHUTDOWN_TIMEOUT_MS,
                warmup_seconds,
                duration_seconds,
                progress_cb=progress_cb,
                metric_cb=metric_cb,
                env_override=env,
            )

        if IS_WINDOWS:
            cmd = [binary_path, lib_name, transport, str(fallback_size)]
        elif os.environ.get("BENCH_TASKSET") == "1":
            cmd = [
                "taskset",
                "-c",
                "1",
                binary_path,
                lib_name,
                transport,
                str(fallback_size),
            ]
        else:
            cmd = [binary_path, lib_name, transport, str(fallback_size)]

        parsed = {}
        expected_metric_count = len(expected_sizes) * 2
        outcome = {
            "parsed": parsed,
            "timed_out": False,
            "returncode": 0,
            "error": "",
        }
        if IS_WINDOWS:
            result = subprocess.run(
                cmd, env=env, capture_output=True, text=True, timeout=timeout_sec
            )
            for line in result.stdout.splitlines():
                prep_line = parse_prep_line(line, transport, expected_sizes)
                if prep_line and progress_cb is not None:
                    line_size, connect_ms, ready_ms = prep_line
                    progress_cb(line_size, connect_ms, ready_ms)
                parsed_line = parse_result_line(line, transport, expected_sizes)
                if not parsed_line:
                    continue
                line_transport, line_size, metric, value = parsed_line
                parsed[f"{line_transport}|{line_size}|{metric}"] = value
                if metric_cb is not None:
                    metric_cb(line_size, metric, value)
                if progress_cb is not None and metric == "throughput":
                    progress_cb(line_size)
            outcome["returncode"] = result.returncode
            return outcome

        proc = subprocess.Popen(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        deadline = time.monotonic() + timeout_sec
        selector = selectors.DefaultSelector()
        if proc.stdout is not None:
            selector.register(proc.stdout, selectors.EVENT_READ)

        timed_out = False
        process_done = False
        while True:
            if process_done:
                break
            now = time.monotonic()
            if now >= deadline:
                timed_out = True
                break

            events = selector.select(timeout=min(1.0, deadline - now))
            if not events:
                if proc.poll() is not None:
                    break
                continue

            for key, _ in events:
                line = key.fileobj.readline()
                if not line:
                    if proc.poll() is not None:
                        process_done = True
                        break
                    continue
                prep_line = parse_prep_line(line, transport, expected_sizes)
                if prep_line and progress_cb is not None:
                    line_size, connect_ms, ready_ms = prep_line
                    progress_cb(line_size, connect_ms, ready_ms)
                parsed_line = parse_result_line(line, transport, expected_sizes)
                if not parsed_line:
                    continue
                line_transport, line_size, metric, value = parsed_line
                parsed[f"{line_transport}|{line_size}|{metric}"] = value
                if metric_cb is not None:
                    metric_cb(line_size, metric, value)
                if progress_cb is not None and metric == "throughput":
                    progress_cb(line_size)

                if expected_metric_count > 0 and len(parsed) >= expected_metric_count:
                    if proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=2.0)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait()
                    selector.close()
                    outcome["returncode"] = 0
                    return outcome

        if timed_out:
            proc.kill()
            rc = proc.wait()
            selector.close()
            outcome["timed_out"] = True
            outcome["returncode"] = rc
            return outcome

        selector.close()

        if proc.stdout is not None:
            tail = proc.stdout.read()
            if tail:
                for line in tail.splitlines():
                    prep_line = parse_prep_line(line, transport, expected_sizes)
                    if prep_line and progress_cb is not None:
                        line_size, connect_ms, ready_ms = prep_line
                        progress_cb(line_size, connect_ms, ready_ms)
                    parsed_line = parse_result_line(line, transport, expected_sizes)
                    if not parsed_line:
                        continue
                    line_transport, line_size, metric, value = parsed_line
                    parsed[f"{line_transport}|{line_size}|{metric}"] = value
                    if metric_cb is not None:
                        metric_cb(line_size, metric, value)
                    if progress_cb is not None and metric == "throughput":
                        progress_cb(line_size)

                    if expected_metric_count > 0 and len(parsed) >= expected_metric_count:
                        if proc.poll() is None:
                            proc.terminate()
                            try:
                                proc.wait(timeout=2.0)
                            except subprocess.TimeoutExpired:
                                proc.kill()
                                proc.wait()
                        outcome["returncode"] = 0
                        return outcome

        rc = proc.wait()
        outcome["returncode"] = rc
        return outcome
    except subprocess.TimeoutExpired:
        return {
            "parsed": {},
            "timed_out": True,
            "returncode": -1,
            "error": "timeout_expired",
        }
    except Exception as exc:
        return {
            "parsed": {},
            "timed_out": False,
            "returncode": -1,
            "error": f"exception_{type(exc).__name__}",
        }


def collect_data(
    binary_name,
    lib_name,
    pattern_name,
    num_runs,
    transports,
    run_cooldown_ms,
    on_size_complete=None,
    msg_sizes=None,
    announce=True,
    show_progress=True,
    allow_no_data=False,
):
    if announce:
        print(f"  > Benchmarking {lib_name} for {pattern_name}...")
    final_stats = {}
    failures = []
    sizes = list(msg_sizes) if msg_sizes is not None else list(MSG_SIZES)

    for tr in transports:
        if not transport_supported_for_pattern(pattern_name, tr):
            continue

        for sz in sizes:
            if show_progress:
                print(f"    Testing {tr} | {sz}B: ", end="", flush=True)
            metrics_raw = {}
            failed_runs = 0
            expected_keys = [
                f"{tr}|{sz}|throughput",
                f"{tr}|{sz}|latency",
            ]

            for i in range(num_runs):
                if show_progress:
                    print(f"{i+1} ", end="", flush=True)
                has_next_run = (i + 1) < num_runs

                def maybe_cooldown():
                    if run_cooldown_ms <= 0 or not has_next_run:
                        return
                    if show_progress:
                        print(f"[cooldown={run_cooldown_ms}ms] ", end="", flush=True)
                    time.sleep(run_cooldown_ms / 1000.0)

                live_metrics = {}

                def on_size_done(size, connect_ms=None, ready_ms=None):
                    if (
                        SHOW_PREP
                        and connect_ms is not None
                        and ready_ms is not None
                    ):
                        print(
                            f"{size}B(c={connect_ms:.0f}ms,r={ready_ms:.0f}ms) ",
                            end="",
                            flush=True,
                        )

                def on_metric(size, metric, value):
                    if not show_progress:
                        return
                    bucket = live_metrics.setdefault(size, {})
                    bucket[metric] = value
                    if "throughput" in bucket and "latency" in bucket:
                        print(
                            f"{size}B[t={bucket['throughput']/1e3:.2f}K,l={bucket['latency']/1e3:.2f}ms] ",
                            end="",
                            flush=True,
                        )
                        live_metrics.pop(size, None)

                run_outcome = run_single_test(
                    binary_name,
                    lib_name,
                    tr,
                    pattern_name,
                    sizes=[sz],
                    progress_cb=on_size_done,
                    metric_cb=on_metric,
                )
                results = run_outcome.get("parsed", {})
                rc = run_outcome.get("returncode", 0)
                timed_out = bool(run_outcome.get("timed_out", False))
                run_error = run_outcome.get("error", "")
                missing = [key for key in expected_keys if key not in results]

                if timed_out:
                    if allow_no_data:
                        maybe_cooldown()
                        continue
                    failed_runs += 1
                    failures.append((pattern_name, lib_name, tr, sz, "timeout"))
                    if FAIL_FAST:
                        if show_progress:
                            print("aborting")
                        return final_stats, failures
                    maybe_cooldown()
                    continue

                if run_error:
                    if allow_no_data:
                        maybe_cooldown()
                        continue
                    failed_runs += 1
                    failures.append((pattern_name, lib_name, tr, sz, run_error))
                    if FAIL_FAST:
                        if show_progress:
                            print("aborting")
                        return final_stats, failures
                    maybe_cooldown()
                    continue

                if not results:
                    if allow_no_data:
                        maybe_cooldown()
                        continue
                    failed_runs += 1
                    reason = f"no_data_rc_{rc}" if rc not in (0, None) else "no_data"
                    failures.append((pattern_name, lib_name, tr, sz, reason))
                    if FAIL_FAST:
                        if show_progress:
                            print("aborting")
                        return final_stats, failures
                    maybe_cooldown()
                    continue

                if missing:
                    if allow_no_data:
                        maybe_cooldown()
                        continue
                    failed_runs += 1
                    for key in missing:
                        metric = key.split("|")[2]
                        suffix = f"_rc_{rc}" if rc not in (0, None) else ""
                        failures.append(
                            (pattern_name, lib_name, tr, sz, f"missing_{metric}{suffix}")
                        )
                    if FAIL_FAST:
                        if show_progress:
                            print("aborting")
                        return final_stats, failures

                for key, value in results.items():
                    metrics_raw.setdefault(key, []).append(value)
                maybe_cooldown()

            for key, vals in metrics_raw.items():
                final_stats[key] = statistics.median(vals) if vals else 0

            if show_progress:
                if failed_runs:
                    print(f"(failures={failed_runs}) ", end="", flush=True)
                print("Done")

            if on_size_complete is not None:
                on_size_complete(tr, sz, final_stats)

    return final_stats, failures


def load_std_cache(path):
    if not path or not os.path.exists(path):
        return {}

    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def save_std_cache(path, cache_data):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cache_data, f, indent=2, sort_keys=True, ensure_ascii=True)


def sanitize_metric_map(raw):
    out = {}
    if not isinstance(raw, dict):
        return out
    for k, v in raw.items():
        try:
            out[str(k)] = float(v)
        except Exception:
            continue
    return out


def sanitize_failures(raw):
    if not isinstance(raw, list):
        return []
    out = []
    for item in raw:
        if isinstance(item, (list, tuple)):
            out.append(tuple(item))
        else:
            out.append((str(item),))
    return out


def get_cached_std_entry(cache_data, pattern_name):
    patterns = cache_data.get("patterns")
    if not isinstance(patterns, dict):
        return None
    entry = patterns.get(pattern_name)
    if not isinstance(entry, dict):
        return None
    std_data = sanitize_metric_map(entry.get("data", {}))
    std_fail = sanitize_failures(entry.get("failures", []))
    return std_data, std_fail


def has_valid_cached_metrics(std_data):
    if not isinstance(std_data, dict):
        return False
    for tr in TRANSPORTS:
        for sz in MSG_SIZES:
            k_t = f"{tr}|{sz}|throughput"
            k_l = f"{tr}|{sz}|latency"
            t = metric_or_none(std_data, k_t)
            l = metric_or_none(std_data, k_l)
            if t is None or l is None:
                return False
            if t <= 0.0 or l <= 0.0:
                return False
    return True


def update_cached_std_entry(cache_data, pattern_name, std_data, std_fail):
    patterns = cache_data.setdefault("patterns", {})
    patterns[pattern_name] = {
        "data": sanitize_metric_map(std_data),
        "failures": [list(x) for x in std_fail],
        "updated_at_utc": datetime.now(timezone.utc).isoformat(),
        "transports": list(TRANSPORTS),
        "msg_sizes": list(MSG_SIZES),
    }


def format_throughput(msgs_per_sec):
    return f"{msgs_per_sec/1e3:6.2f} Kmsg/s"


def metric_or_none(metric_map, key):
    if key not in metric_map:
        return None
    try:
        return float(metric_map[key])
    except Exception:
        return None


def print_transport_report_header(transport):
    size_w = 6
    metric_w = 10
    val_w = 16
    diff_w = 9
    print(f"\n### Transport: {transport}")
    print(
        f"| {'Size':<{size_w}} | {'Metric':<{metric_w}} | {'Standard libzmq':>{val_w}} | {'zlink':>{val_w}} | {'Diff (%)':>{diff_w}} |"
    )
    print(
        f"|{'-' * (size_w + 2)}|{'-' * (metric_w + 2)}|{'-' * (val_w + 2)}|{'-' * (val_w + 2)}|{'-' * (diff_w + 2)}|"
    )


def print_size_comparison_rows(transport, size, std_data, zlk_data, std_available=True):
    size_w = 6
    metric_w = 10
    val_w = 16
    diff_w = 9

    k_t = f"{transport}|{size}|throughput"
    k_l = f"{transport}|{size}|latency"
    zlk_t = metric_or_none(zlk_data, k_t)
    zlk_l = metric_or_none(zlk_data, k_l)

    zlk_t_s = format_throughput(zlk_t) if zlk_t is not None else "N/A"
    zlk_l_s = f"{zlk_l:8.2f} ms" if zlk_l is not None else "N/A"
    if std_available:
        std_t = metric_or_none(std_data, k_t)
        std_l = metric_or_none(std_data, k_l)
        std_t_s = format_throughput(std_t) if std_t is not None else "N/A"
        std_l_s = f"{std_l:8.2f} ms" if std_l is not None else "N/A"
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
    else:
        std_t_s = "N/A"
        std_l_s = "N/A"
        t_diff_s = "N/A"
        l_diff_s = "N/A"

    print(
        f"| {f'{size}B':<{size_w}} | {'Throughput':<{metric_w}} | {std_t_s:>{val_w}} | {zlk_t_s:>{val_w}} | {t_diff_s:>{diff_w}} |"
    )
    print(
        f"| {f'{size}B':<{size_w}} | {'Latency':<{metric_w}} | {std_l_s:>{val_w}} | {zlk_l_s:>{val_w}} | {l_diff_s:>{diff_w}} |"
    )


def print_pattern_report(std_data, zlk_data, std_available=True):
    for tr in TRANSPORTS:
        print_transport_report_header(tr)
        for sz in MSG_SIZES:
            print_size_comparison_rows(tr, sz, std_data, zlk_data, std_available)


def normalize_pattern_name(raw):
    if raw is None:
        return None
    token = str(raw).strip().upper()
    if not token:
        return None
    if token == "ALL":
        return "ALL"

    aliases = {
        "DEALER_DEALER": "MULTI_DEALER_DEALER",
        "DEALER_ROUTER": "MULTI_DEALER_ROUTER",
        "ROUTER_ROUTER": "MULTI_ROUTER_ROUTER",
        "PUBSUB": "MULTI_PUBSUB",
        "STREAM": "MULTI_STREAM",
    }
    return aliases.get(token)


def parse_args():
    usage = (
        "Usage: multi/run_comparison.py [PATTERN] [options]\n\n"
        "Options:\n"
        "  --zlink-only            Re-measure only zlink (use cached libzmq when available)\n"
        "  --refresh-std-cache     Refresh libzmq cache with current measurements\n"
        "  --std-cache-file PATH   libzmq cache file path (default: platform cache in multi/)\n"
        "  --runs N                Iterations per configuration (default: 3)\n"
        "  --run-cooldown-ms N     Sleep between runs (default: BENCH_MULTI_RUN_COOLDOWN_MS)\n"
        "  --build-dir PATH        Build directory\n"
        "  --pin-cpu               Pin CPU core during benchmarks (Linux taskset)\n"
        "  -h, --help              Show this help\n"
        "\n"
        "PATTERN:\n"
        "  dealer_dealer | dealer_router | router_router | pubsub | stream\n"
        "  Missing benchmark binaries are auto-built via cmake --build.\n"
        "\n"
        "Env:\n"
        "  BENCH_TRANSPORTS=tcp\n"
        "  BENCH_MSG_SIZES=1024\n"
        "  BENCH_MULTI_CLIENTS=100 (stream=10000)\n"
        "  PERF_MULTI_STREAM_CONNECT_BATCH=256 (recommended for stream=10000)\n"
        "  BENCH_MULTI_HWM=100 (stream=10)\n"
        "  BENCH_MULTI_SNDTIMEO_MS=200\n"
        "  BENCH_MULTI_RCVTIMEO_MS=200\n"
        "  BENCH_MULTI_MONITOR_HWM_BYTES=1000\n"
        "  BENCH_MULTI_CONNECT_READY_TIMEOUT_MS=5000\n"
        "  BENCH_MULTI_WARMUP_SECONDS=2\n"
        "  BENCH_MULTI_DURATION_SECONDS=5\n"
        "  BENCH_MULTI_CONNECT_CONCURRENCY=128\n"
        "  BENCH_MULTI_RUN_COOLDOWN_MS=3000\n"
        "  BENCH_MULTI_PRINT_FINAL_REPORT=1\n"
        "  BENCH_IO_THREADS=4\n"
        "  BENCH_MULTI_TIMEOUT_SECONDS=300\n"
    )

    pattern = "ALL"
    num_runs = DEFAULT_NUM_RUNS
    run_cooldown_ms = DEFAULT_RUN_COOLDOWN_MS
    build_dir = ""
    zlink_only = False
    pin_cpu = False
    refresh_std_cache = False
    std_cache_file = ""

    i = 1
    while i < len(sys.argv):
        arg = sys.argv[i]
        if arg in ("-h", "--help"):
            print(usage)
            sys.exit(0)
        if arg == "--zlink-only":
            zlink_only = True
        elif arg == "--refresh-std-cache":
            refresh_std_cache = True
        elif arg == "--std-cache-file":
            if i + 1 >= len(sys.argv):
                print("Error: --std-cache-file requires a value", file=sys.stderr)
                sys.exit(1)
            std_cache_file = sys.argv[i + 1]
            i += 1
        elif arg == "--pin-cpu":
            pin_cpu = True
        elif arg == "--runs":
            if i + 1 >= len(sys.argv):
                print("Error: --runs requires a value", file=sys.stderr)
                sys.exit(1)
            num_runs = int(sys.argv[i + 1])
            i += 1
        elif arg.startswith("--runs="):
            num_runs = int(arg.split("=", 1)[1])
        elif arg == "--run-cooldown-ms":
            if i + 1 >= len(sys.argv):
                print("Error: --run-cooldown-ms requires a value", file=sys.stderr)
                sys.exit(1)
            run_cooldown_ms = max(0, int(sys.argv[i + 1]))
            i += 1
        elif arg.startswith("--run-cooldown-ms="):
            run_cooldown_ms = max(0, int(arg.split("=", 1)[1]))
        elif arg == "--build-dir":
            if i + 1 >= len(sys.argv):
                print("Error: --build-dir requires a value", file=sys.stderr)
                sys.exit(1)
            build_dir = sys.argv[i + 1]
            i += 1
        elif not arg.startswith("--") and pattern == "ALL":
            normalized = normalize_pattern_name(arg)
            if not normalized:
                print(
                    f"Error: unsupported pattern '{arg}'. "
                    "Use router_router.",
                    file=sys.stderr,
                )
                sys.exit(1)
            pattern = normalized
        i += 1

    if num_runs < 1:
        print("Error: --runs must be >= 1", file=sys.stderr)
        sys.exit(1)

    return (
        pattern,
        num_runs,
        run_cooldown_ms,
        build_dir,
        zlink_only,
        pin_cpu,
        refresh_std_cache,
        std_cache_file,
    )


def main():
    global BUILD_DIR, ZLINK_LIB_DIR, base_env

    (
        pattern_req,
        num_runs,
        run_cooldown_ms,
        build_dir,
        zlink_only,
        pin_cpu,
        refresh_std_cache,
        std_cache_file,
    ) = parse_args()
    BUILD_DIR = normalize_build_dir(build_dir) if build_dir else normalize_build_dir(BUILD_DIR)
    ZLINK_LIB_DIR = derive_zlink_lib_dir(BUILD_DIR)
    cache_file = os.path.abspath(std_cache_file) if std_cache_file else DEFAULT_STD_CACHE_FILE
    std_cache = load_std_cache(cache_file)

    if pin_cpu:
        base_env["BENCH_TASKSET"] = "1"

    if zlink_only and refresh_std_cache:
        print(
            "Info: --zlink-only with --refresh-std-cache will refresh libzmq baselines first.",
            flush=True,
        )

    comparisons = [
        ("comp_std_zmq_multi_dealer_dealer_client", "comp_zlink_multi_dealer_dealer_client", "MULTI_DEALER_DEALER"),
        ("comp_std_zmq_multi_dealer_router_client", "comp_zlink_multi_dealer_router_client", "MULTI_DEALER_ROUTER"),
        ("comp_std_zmq_multi_router_router_client", "comp_zlink_multi_router_router_client", "MULTI_ROUTER_ROUTER"),
        ("comp_std_zmq_multi_pubsub_client", "comp_zlink_multi_pubsub_client", "MULTI_PUBSUB"),
        ("comp_std_zmq_multi_stream_client", "comp_zlink_multi_stream", "MULTI_STREAM"),
    ]

    supported = sorted({p for _, _, p in comparisons})
    if pattern_req != "ALL" and pattern_req not in supported:
        print(
            f"Error: unsupported pattern '{pattern_req}'. Supported: {', '.join(supported)}",
            file=sys.stderr,
        )
        return 2

    selected_comparisons = select_comparisons(comparisons, pattern_req)
    missing_cache_patterns = []
    for _, _, p_name in selected_comparisons:
        cached_entry = get_cached_std_entry(std_cache, p_name)
        if cached_entry is None or not has_valid_cached_metrics(cached_entry[0]):
            missing_cache_patterns.append(p_name)
    need_std_baseline = (not zlink_only) or refresh_std_cache or bool(
        missing_cache_patterns
    )

    expected_bins = expected_runtime_binaries(selected_comparisons, need_std_baseline)
    missing = collect_missing_binaries(BUILD_DIR, expected_bins)
    no_autobuild = (
        os.environ.get("BENCH_NO_AUTOBUILD") == "1"
        or os.environ.get("PERF_NO_AUTOBUILD") == "1"
    )

    if missing:
        if no_autobuild:
            print(
                f"Error: missing benchmark binaries in {BUILD_DIR} and auto-build is disabled.",
                file=sys.stderr,
            )
            for name in missing:
                print(f"  - {name}{EXE_SUFFIX}", file=sys.stderr)
            return 2

        cmake_build_dir = derive_cmake_build_dir(BUILD_DIR)
        if not cmake_build_dir:
            print(
                f"Error: missing benchmark binaries in {BUILD_DIR} and failed to derive CMake build dir.",
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

        BUILD_DIR = normalize_build_dir(runtime_bin_dir_from_cmake_dir(cmake_build_dir))
        ZLINK_LIB_DIR = derive_zlink_lib_dir(BUILD_DIR)
        build_targets = collect_multi_build_targets(
            selected_comparisons, need_std_baseline
        )
        build_rc = run_cmake_build(cmake_build_dir, build_targets)
        if build_rc != 0:
            print(
                f"Error: auto-build failed with exit code {build_rc}",
                file=sys.stderr,
            )
            return build_rc

        missing = collect_missing_binaries(BUILD_DIR, expected_bins)

    if missing:
        print(f"Error: missing benchmark binaries in {BUILD_DIR}:", file=sys.stderr)
        for name in missing:
            print(f"  - {name}{EXE_SUFFIX}", file=sys.stderr)
        return 2

    any_failure = False
    all_failures = []
    cache_updated = False
    print_final_report = os.environ.get("BENCH_MULTI_PRINT_FINAL_REPORT", "0") == "1"

    printed_pattern = False
    for pattern_index, (std_bin, zlk_bin, p_name) in enumerate(selected_comparisons):

        if printed_pattern:
            print("")
            print(PATTERN_SEPARATOR)
            print("")
        else:
            print("")
        print(f"## PATTERN: {p_name}")
        printed_pattern = True

        std_data = {}
        std_fail = []
        zlk_data = {}
        zlk_fail = []
        live_rows = 0

        cached = get_cached_std_entry(std_cache, p_name)
        cached_valid = cached is not None and has_valid_cached_metrics(cached[0])
        std_from_cache = False
        if zlink_only:
            if refresh_std_cache or not cached_valid:
                reason = (
                    "refresh requested"
                    if refresh_std_cache
                    else "baseline missing/invalid"
                )
                print(
                    f"  > Cached libzmq {reason} for {p_name}; "
                    "measuring and caching libzmq baseline now."
                )
            else:
                std_data, std_fail = cached
                print(f"  > Using cached libzmq for {p_name} from {cache_file}")
                std_from_cache = True
        else:
            std_from_cache = False

        if not std_from_cache:
            print(f"  > Benchmarking libzmq for {p_name}...")
        print(f"  > Benchmarking zlink for {p_name}...")

        for transport_index, tr in enumerate(TRANSPORTS):
            if not transport_supported_for_pattern(p_name, tr):
                if (
                    TRANSPORT_TRANSITION_MS > 0
                    and transport_index + 1 < len(TRANSPORTS)
                ):
                    time.sleep(float(TRANSPORT_TRANSITION_MS) / 1000.0)
                continue

            print_transport_report_header(tr)
            for sz in MSG_SIZES:
                if not std_from_cache:
                    std_partial, std_partial_fail = collect_data(
                        std_bin,
                        "libzmq",
                        p_name,
                        num_runs,
                        [tr],
                        run_cooldown_ms,
                        msg_sizes=[sz],
                        announce=False,
                        show_progress=False,
                        allow_no_data=False,
                    )
                    std_data.update(std_partial)
                    std_fail.extend(std_partial_fail)

                zlk_partial, zlk_partial_fail = collect_data(
                    zlk_bin,
                    "zlink",
                    p_name,
                    num_runs,
                    [tr],
                    run_cooldown_ms,
                    msg_sizes=[sz],
                    announce=False,
                    show_progress=False,
                    allow_no_data=False,
                )
                zlk_data.update(zlk_partial)
                zlk_fail.extend(zlk_partial_fail)

                print_size_comparison_rows(
                    tr,
                    sz,
                    std_data,
                    zlk_data,
                    std_available=True,
                )
                live_rows += 1

            if (
                TRANSPORT_TRANSITION_MS > 0
                and transport_index + 1 < len(TRANSPORTS)
            ):
                time.sleep(float(TRANSPORT_TRANSITION_MS) / 1000.0)

        if zlink_only and not std_from_cache:
            update_cached_std_entry(std_cache, p_name, std_data, std_fail)
            cache_updated = True
        if (not zlink_only) and (refresh_std_cache or cached is None):
            update_cached_std_entry(std_cache, p_name, std_data, std_fail)
            cache_updated = True

        if print_final_report or live_rows == 0:
            print_pattern_report(std_data, zlk_data, std_available=True)
        all_failures.extend(std_fail)
        all_failures.extend(zlk_fail)

        if std_fail or zlk_fail:
            any_failure = True

        if (
            PATTERN_TRANSITION_MS > 0
            and pattern_index + 1 < len(selected_comparisons)
        ):
            time.sleep(float(PATTERN_TRANSITION_MS) / 1000.0)

    if cache_updated:
        std_cache["meta"] = {
            "build_dir": BUILD_DIR,
            "libzmq_lib_dir": LIBZMQ_LIB_DIR,
            "platform_tag": _platform_tag,
            "arch_tag": _arch_tag,
            "updated_at_utc": datetime.now(timezone.utc).isoformat(),
        }
        save_std_cache(cache_file, std_cache)
        print(f"Saved libzmq cache: {cache_file}")

    if all_failures:
        print("\n## Failures")
        for failure in all_failures:
            if len(failure) >= 5:
                pattern, lib_name, tr, sz, reason = failure[:5]
                print(f"- {pattern} {lib_name} {tr} {sz}B: {reason}")
            else:
                print(f"- {failure}")
    return 1 if any_failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
