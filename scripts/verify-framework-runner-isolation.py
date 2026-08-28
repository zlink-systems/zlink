#!/usr/bin/env python3
"""Verify cross-language sample and E2E runner isolation contracts."""

from __future__ import annotations

import itertools
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class RangeSource:
    language: str
    suite: str
    purpose: str
    path: str
    minimum_pattern: str
    maximum_pattern: str


@dataclass(frozen=True)
class LockSource:
    language: str
    path: str
    pattern: str


@dataclass(frozen=True)
class RunnerInventory:
    language: str
    root: str
    expected: tuple[str, ...]
    lock_marker: str


@dataclass(frozen=True)
class SampleRunnerInventory:
    language: str
    root: str
    directory_suffix: str
    powershell_samples: tuple[str, ...]


@dataclass(frozen=True)
class AggregateRunner:
    language: str
    path: str
    child_call: str
    active_pid_marker: str | None = None
    wait_marker: str | None = None


@dataclass(frozen=True)
class ShellRedisHelper:
    path: str
    start_function: str
    attempt_function: str
    cleanup_function: str


RANGES = (
    RangeSource("cpp", "sample", "redis", "framework/languages/cpp/samples/redis-common.sh",
                r"^ZLINK_CPP_SAMPLE_REDIS_PORT_MIN=(\d+)$",
                r"^ZLINK_CPP_SAMPLE_REDIS_PORT_MAX=(\d+)$"),
    RangeSource("cpp", "sample", "application", "framework/languages/cpp/samples/redis-common.sh",
                r"^ZLINK_CPP_SAMPLE_APP_PORT_MIN=(\d+)$",
                r"^ZLINK_CPP_SAMPLE_APP_PORT_MAX=(\d+)$"),
    RangeSource("cpp", "e2e", "redis", "framework/languages/cpp/e2e/redis-common.sh",
                r"^ZLINK_CPP_E2E_REDIS_PORT_MIN=(\d+)$",
                r"^ZLINK_CPP_E2E_REDIS_PORT_MAX=(\d+)$"),
    RangeSource("cpp", "e2e", "application", "framework/languages/cpp/e2e/redis-common.sh",
                r"^ZLINK_CPP_E2E_APP_PORT_MIN=(\d+)$",
                r"^ZLINK_CPP_E2E_APP_PORT_MAX=(\d+)$"),
    RangeSource("dotnet", "sample", "redis", "framework/languages/dotnet/samples/sample_runner.ps1",
                r"^\s*\$redisMinimumPort\s*=\s*(\d+)$",
                r"^\s*\$redisMaximumPort\s*=\s*(\d+)$"),
    RangeSource("dotnet", "sample", "application", "framework/languages/dotnet/samples/sample_runner.ps1",
                r"^\s*\$applicationMinimumPort\s*=\s*(\d+)$",
                r"^\s*\$applicationMaximumPort\s*=\s*(\d+)$"),
    RangeSource("dotnet", "e2e", "redis", "framework/languages/dotnet/e2e/redis-common.sh",
                r"^\s*local redis_min_port=(\d+)$",
                r"^\s*local redis_max_port=(\d+)$"),
    RangeSource("dotnet", "e2e", "application", "framework/languages/dotnet/e2e/redis-common.sh",
                r"^minimum_port = (\d+)$",
                r"^maximum_port = (\d+)$"),
    RangeSource("java", "sample", "redis", "framework/languages/java/samples/runner-common.sh",
                r"^\s*ZLINK_SAMPLE_REDIS_PORT_MIN=(24000)$",
                r"^\s*ZLINK_SAMPLE_REDIS_PORT_MAX=(24099)$"),
    RangeSource("java", "sample", "application", "framework/languages/java/samples/runner-common.sh",
                r"^\s*ZLINK_SAMPLE_APP_PORT_MIN=(24100)$",
                r"^\s*ZLINK_SAMPLE_APP_PORT_MAX=(25999)$"),
    RangeSource("kotlin", "sample", "redis", "framework/languages/java/samples/runner-common.sh",
                r"^\s*ZLINK_SAMPLE_REDIS_PORT_MIN=(26000)$",
                r"^\s*ZLINK_SAMPLE_REDIS_PORT_MAX=(26099)$"),
    RangeSource("kotlin", "sample", "application", "framework/languages/java/samples/runner-common.sh",
                r"^\s*ZLINK_SAMPLE_APP_PORT_MIN=(26100)$",
                r"^\s*ZLINK_SAMPLE_APP_PORT_MAX=(27999)$"),
    RangeSource("java", "e2e", "redis", "framework/languages/java/e2e-runner-common.sh",
                r"^\s*ZLINK_E2E_REDIS_PORT_MIN=(34000)$",
                r"^\s*ZLINK_E2E_REDIS_PORT_MAX=(34099)$"),
    RangeSource("java", "e2e", "application", "framework/languages/java/e2e-runner-common.sh",
                r"^\s*ZLINK_E2E_APP_PORT_MIN=(34100)$",
                r"^\s*ZLINK_E2E_APP_PORT_MAX=(35999)$"),
    RangeSource("kotlin", "e2e", "redis", "framework/languages/java/e2e-runner-common.sh",
                r"^\s*ZLINK_E2E_REDIS_PORT_MIN=(36000)$",
                r"^\s*ZLINK_E2E_REDIS_PORT_MAX=(36099)$"),
    RangeSource("kotlin", "e2e", "application", "framework/languages/java/e2e-runner-common.sh",
                r"^\s*ZLINK_E2E_APP_PORT_MIN=(36100)$",
                r"^\s*ZLINK_E2E_APP_PORT_MAX=(37999)$"),
    RangeSource("node", "sample", "redis", "framework/languages/node/samples/run-sample.mjs",
                r"^const redisPortRange = \{ min: (\d+), max: \d+ \};$",
                r"^const redisPortRange = \{ min: \d+, max: (\d+) \};$"),
    RangeSource("node", "sample", "application", "framework/languages/node/samples/run-sample.mjs",
                r"^const applicationPortRange = \{ min: (\d+), max: \d+ \};$",
                r"^const applicationPortRange = \{ min: \d+, max: (\d+) \};$"),
    RangeSource("node", "e2e", "redis", "framework/languages/node/e2e/redis-container.sh",
                r"^NODE_E2E_REDIS_PORT_MIN=(\d+)$",
                r"^NODE_E2E_REDIS_PORT_MAX=(\d+)$"),
    RangeSource("node", "e2e", "application", "framework/languages/node/e2e/runner-common.sh",
                r"^NODE_E2E_APPLICATION_PORT_MIN=(\d+)$",
                r"^NODE_E2E_APPLICATION_PORT_MAX=(\d+)$"),
)

LOCKS = (
    LockSource("cpp", "framework/languages/cpp/e2e/redis-common.sh",
               r"^ZLINK_CPP_E2E_RUN_LOCK_PATH=(/tmp/[^\s]+)$"),
    LockSource("dotnet", "framework/languages/dotnet/e2e/redis-common.sh",
               r'^\s*local lock_path="(/tmp/[^\"]+)"$'),
    LockSource("java", "framework/languages/java/e2e-runner-common.sh",
               r'^\s*ZLINK_E2E_RUN_LOCK_PATH="(/tmp/zlink-framework-java-e2e-run\.lock)"$'),
    LockSource("kotlin", "framework/languages/java/e2e-runner-common.sh",
               r'^\s*ZLINK_E2E_RUN_LOCK_PATH="(/tmp/zlink-framework-kotlin-e2e-run\.lock)"$'),
    LockSource("node", "framework/languages/node/e2e/runner-common.sh",
               r'^NODE_E2E_LANGUAGE_LOCK_FILE="(/tmp/[^\"]+)"$'),
)

RUNNER_INVENTORIES = (
    RunnerInventory(
        "cpp",
        "framework/languages/cpp/e2e",
        (
            "AutomaticTurnDispatch/run_e2e.sh",
            "ChannelEgressRouting/run_e2e.sh",
            "DiscoveryRegistryHa/run_e2e.sh",
            "InstanceSpot/run_e2e.sh",
            "ObservabilityOps/run_e2e.sh",
            "PubSub/run_e2e.sh",
            "RegistrationCodec/run_e2e.sh",
            "RegistryMessaging/run_e2e.sh",
            "RegistryMessaging/run_rm_a7_global_identity.sh",
            "RelocationRetry/run_e2e.sh",
            "ResilienceLifecycle/run_e2e.sh",
            "RuntimeMonitoring/run_e2e.sh",
            "SpotActorTransfer/run_e2e.sh",
            "SpotService/run_e2e.sh",
            "SubmitAdmission/run_e2e.sh",
            "ToActorMessaging/run_e2e.sh",
        ),
        'zlink_cpp_e2e_acquire_run_lock',
    ),
    RunnerInventory(
        "dotnet",
        "framework/languages/dotnet/e2e",
        (
            "AutomaticTurnDispatch/run_e2e.sh",
            "ChannelEgressRouting/run_e2e.sh",
            "InstanceSpot/run_e2e.sh",
            "LocationMessaging/run_e2e.sh",
            "ObservabilityOps/run_e2e.sh",
            "PubSub/run_e2e.sh",
            "RegistrationCodec/run_e2e.sh",
            "ResilienceLifecycle/run_e2e.sh",
            "RuntimeMonitoring/run_e2e.sh",
            "SpotActorTransfer/run_e2e.sh",
            "SpotService/run_e2e.sh",
            "StoreFailure/run_e2e.sh",
            "SubmitAdmission/run_e2e.sh",
            "ToActorMessaging/run_e2e.sh",
        ),
        'zlink_dotnet_e2e_acquire_run_lock "$0" "$@"',
    ),
    RunnerInventory(
        "java",
        "framework/languages/java/e2e",
        (
            "AutomaticTurnDispatch/run_e2e.sh",
            "ChannelEgressRouting/run_e2e.sh",
            "InstanceSpot/run_e2e.sh",
            "ObservabilityOps/run_a5_e2e.sh",
            "ObservabilityOps/run_c_e2e.sh",
            "ObservabilityOps/run_e2e.sh",
            "PubSub/run_e2e.sh",
            "RegistrationCodec/run_e2e.sh",
            "RegistryMessaging/run_e2e.sh",
            "ResilienceLifecycle/run_e2e.sh",
            "RuntimeMonitoring/run_e2e.sh",
            "SpotActorTransfer/run_e2e.sh",
            "SpotService/run_e2e.sh",
            "StoreFailure/run_e2e.sh",
            "SubmitAdmission/run_e2e.sh",
            "ToActorMessaging/run_e2e.sh",
        ),
        'zlink_e2e_initialize java "$0" "$@"',
    ),
    RunnerInventory(
        "kotlin",
        "framework/languages/java/e2e-kotlin",
        (
            "AutomaticTurnDispatch/run_e2e.sh",
            "ChannelEgressRouting/run_e2e.sh",
            "DiscoveryRegistryHa/run_e2e.sh",
            "InstanceSpot/run_e2e.sh",
            "ObservabilityOps/run_a5_e2e.sh",
            "ObservabilityOps/run_e2e.sh",
            "PubSub/run_e2e.sh",
            "RegistrationCodec/run_e2e.sh",
            "RegistryMessaging/run_e2e.sh",
            "ResilienceLifecycle/run_e2e.sh",
            "RuntimeMonitoring/run_e2e.sh",
            "SpotActorTransfer/run_e2e.sh",
            "SpotService/run_e2e.sh",
            "StoreFailure/run_e2e.sh",
            "SubmitAdmission/run_e2e.sh",
            "ToActorMessaging/run_e2e.sh",
        ),
        'zlink_e2e_initialize kotlin "$0" "$@"',
    ),
    RunnerInventory(
        "node",
        "framework/languages/node/e2e",
        (
            "AutomaticTurnDispatch/run_e2e.sh",
            "ChannelEgressRouting/run_e2e.sh",
            "DiscoveryRegistryHa/run_e2e.sh",
            "InstanceSpot/run_e2e.sh",
            "ObservabilityOps/run_e2e.sh",
            "PubSub/run_e2e.sh",
            "RegistrationCodec/run_e2e.sh",
            "RegistryMessaging/run_e2e.sh",
            "ResilienceLifecycle/run_e2e.sh",
            "RuntimeMonitoring/run_e2e.sh",
            "SpotActorTransfer/run_e2e.sh",
            "SpotService/run_e2e.sh",
            "SubmitAdmission/run_e2e.sh",
            "ToActorMessaging/run_e2e.sh",
        ),
        'serialize_node_e2e_run "$0" "$@"',
    ),
)

SAMPLE_NAMES = (
    "Bingo",
    "DeliveryDispatch",
    "GameQuest",
    "ShoppingMall",
    "SupportChat",
    "TicTacToe",
    "ZoneWorld",
)

SAMPLE_RUNNER_INVENTORIES = (
    SampleRunnerInventory(
        "cpp",
        "framework/languages/cpp/samples",
        "",
        ("Bingo", "DeliveryDispatch", "GameQuest", "ShoppingMall",
         "SupportChat", "TicTacToe"),
    ),
    SampleRunnerInventory(
        "dotnet",
        "framework/languages/dotnet/samples",
        "",
        SAMPLE_NAMES,
    ),
    SampleRunnerInventory(
        "java",
        "framework/languages/java/samples/java",
        "",
        ("Bingo", "DeliveryDispatch", "GameQuest", "ShoppingMall",
         "SupportChat", "TicTacToe"),
    ),
    SampleRunnerInventory(
        "kotlin",
        "framework/languages/java/samples/kotlin",
        "",
        ("Bingo", "DeliveryDispatch", "GameQuest", "ShoppingMall",
         "SupportChat", "TicTacToe"),
    ),
    SampleRunnerInventory(
        "node",
        "framework/languages/node/samples",
        ".Ts",
        SAMPLE_NAMES,
    ),
)

AGGREGATE_RUNNERS = (
    AggregateRunner(
        "cpp",
        "framework/languages/cpp/e2e/run_e2e_all.sh",
        'run_config_once "${config}" "${scenario}" "${start_order}"',
    ),
    AggregateRunner(
        "dotnet",
        "framework/languages/dotnet/e2e/run_e2e_all.sh",
        'run_config "$config" "$scenario"',
        'active_config_pid="$!"',
        'wait "$active_config_pid"',
    ),
    AggregateRunner(
        "java",
        "framework/languages/java/e2e/run_e2e_all.sh",
        'run_scenario_with_retry "${scenario}"',
        'active_scenario_pid="$!"',
        'wait "${active_scenario_pid}"',
    ),
    AggregateRunner(
        "kotlin",
        "framework/languages/java/e2e-kotlin/run_e2e_all.sh",
        'run_scenario_with_retry "${scenario}" "${selector}"',
    ),
    AggregateRunner(
        "node",
        "framework/languages/node/e2e/run_e2e_all.sh",
        'run_config_with_retry "${config}"',
        'active_config_pid="$!"',
        'wait "${active_config_pid}"',
    ),
)

SHELL_REDIS_HELPERS = (
    ShellRedisHelper(
        "framework/languages/cpp/samples/redis-common.sh",
        "zlink_redis_start_scoped",
        "zlink_redis_remove_attempt",
        "zlink_redis_remove_by_id",
    ),
    ShellRedisHelper(
        "framework/languages/dotnet/samples/redis-common.sh",
        "zlink_redis_start_scoped",
        "zlink_redis_remove_attempt",
        "zlink_redis_remove_by_id",
    ),
    ShellRedisHelper(
        "framework/languages/java/samples/runner-common.sh",
        "zlink_redis_start_scoped",
        "zlink_redis_remove_attempt",
        "zlink_redis_remove_by_id",
    ),
    ShellRedisHelper(
        "framework/languages/cpp/e2e/redis-common.sh",
        "zlink_redis_start_scoped",
        "zlink_redis_remove_attempt",
        "zlink_redis_remove_by_id",
    ),
    ShellRedisHelper(
        "framework/languages/dotnet/e2e/redis-common.sh",
        "zlink_redis_start_scoped",
        "zlink_redis_remove_attempt",
        "zlink_redis_remove_by_id",
    ),
    ShellRedisHelper(
        "framework/languages/java/e2e-redis-common.sh",
        "zlink_redis_start_scoped",
        "zlink_redis_remove_attempt",
        "zlink_redis_remove_by_id",
    ),
    ShellRedisHelper(
        "framework/languages/node/e2e/redis-container.sh",
        "start_redis_container",
        "remove_redis_attempt",
        "remove_redis_attempt",
    ),
)

LANGUAGE_LABELS = {
    "cpp": "C++",
    "dotnet": ".NET",
    "java": "Java",
    "kotlin": "Kotlin",
    "node": "Node.js",
}

README_PATHS = {
    "sample": (
        "framework/doc/framework/common/sample/README.ko.md",
        "framework/doc/framework/common/sample/README.en.md",
    ),
    "e2e": (
        "framework/doc/framework/common/e2e/README.ko.md",
        "framework/doc/framework/common/e2e/README.en.md",
    ),
}

TEMPLATE_PAIRS = (
    (
        "framework/doc/framework/common/sample/runner-templates/redis-common.template.sh",
        "framework/doc/framework/common/sample/runner-templates/run_sample.template.sh",
    ),
    (
        "framework/doc/framework/common/e2e/runner-templates/redis-common.template.sh",
        "framework/doc/framework/common/e2e/runner-templates/run_e2e.template.sh",
    ),
)


def exact_match(path: Path, pattern: str) -> str:
    matches = re.findall(pattern, path.read_text(encoding="utf-8"), re.MULTILINE)
    if len(matches) != 1:
        raise ValueError(
            f"{path.relative_to(ROOT)}: expected one match for {pattern!r}, "
            f"found {len(matches)}"
        )
    return matches[0]


def require_once(path: Path, text: str) -> None:
    count = path.read_text(encoding="utf-8").count(text)
    if count != 1:
        raise ValueError(
            f"{path.relative_to(ROOT)}: expected one occurrence of {text!r}, "
            f"found {count}"
        )


def require_text(path: Path, text: str) -> None:
    if text not in path.read_text(encoding="utf-8"):
        raise ValueError(f"{path.relative_to(ROOT)}: missing {text!r}")


def require_absent(path: Path, pattern: str, description: str) -> None:
    source = path.read_text(encoding="utf-8")
    if re.search(pattern, source, re.MULTILINE):
        raise ValueError(f"{path.relative_to(ROOT)}: {description}")


def integer_matches(path: Path, pattern: str) -> list[int]:
    return [
        int(value)
        for value in re.findall(pattern, path.read_text(encoding="utf-8"), re.MULTILINE)
    ]


def shell_function(path: Path, name: str) -> str:
    source = path.read_text(encoding="utf-8")
    start = re.search(
        rf"(?m)^{re.escape(name)}\(\)\s*\{{\s*$",
        source,
    )
    if start is None:
        raise ValueError(f"{path.relative_to(ROOT)}: missing shell function {name}")
    following = re.search(r"(?m)^[a-zA-Z_][a-zA-Z0-9_]*\(\)\s*\{\s*$", source[start.end():])
    end = len(source) if following is None else start.end() + following.start()
    return source[start.start():end]


def verify_mirrored_ranges(
    resolved_ranges: dict[tuple[str, str, str], tuple[int, int]],
) -> None:
    mirrors = (
        RangeSource(
            "cpp", "sample", "redis",
            "framework/languages/cpp/samples/redis-common.ps1",
            r"^\$script:ZlinkCppSampleRedisPortMin\s*=\s*(\d+)$",
            r"^\$script:ZlinkCppSampleRedisPortMax\s*=\s*(\d+)$",
        ),
        RangeSource(
            "cpp", "sample", "application",
            "framework/languages/cpp/samples/redis-common.ps1",
            r"^\$script:ZlinkCppSampleAppPortMin\s*=\s*(\d+)$",
            r"^\$script:ZlinkCppSampleAppPortMax\s*=\s*(\d+)$",
        ),
        RangeSource(
            "dotnet", "sample", "redis",
            "framework/languages/dotnet/samples/redis-common.sh",
            r"^\s*local redis_min_port=(\d+)$",
            r"^\s*local redis_max_port=(\d+)$",
        ),
        RangeSource(
            "node", "e2e", "application",
            "framework/languages/node/e2e/port-picker.js",
            r"^const MIN_PORT = (\d+);$",
            r"^const MAX_PORT = (\d+);$",
        ),
    )
    for mirror in mirrors:
        actual = (
            int(exact_match(ROOT / mirror.path, mirror.minimum_pattern)),
            int(exact_match(ROOT / mirror.path, mirror.maximum_pattern)),
        )
        expected = resolved_ranges[(mirror.language, mirror.suite, mirror.purpose)]
        if actual != expected:
            raise ValueError(
                f"{mirror.path}: mirrored {mirror.language} {mirror.suite} "
                f"{mirror.purpose} range is {actual[0]}-{actual[1]}, expected "
                f"{expected[0]}-{expected[1]}"
            )

    dotnet_app_range = resolved_ranges[("dotnet", "sample", "application")]
    dotnet_sample_root = ROOT / "framework/languages/dotnet/samples"
    dotnet_shell_runners = sorted(dotnet_sample_root.glob("*/run_sample.sh"))
    if len(dotnet_shell_runners) != 7:
        raise ValueError(
            "framework/languages/dotnet/samples: expected seven Bash sample "
            f"runners, found {len(dotnet_shell_runners)}"
        )
    for runner in dotnet_shell_runners:
        pairs = re.findall(
            r"random\.randint\((\d+),\s*(\d+)\)",
            runner.read_text(encoding="utf-8"),
        )
        if pairs != [(str(dotnet_app_range[0]), str(dotnet_app_range[1]))]:
            raise ValueError(
                f"{runner.relative_to(ROOT)}: expected one application port "
                f"mirror for {dotnet_app_range[0]}-{dotnet_app_range[1]}, "
                f"found {pairs}"
            )

    jvm_powershell = ROOT / "framework/languages/java/samples/redis-common.ps1"
    jvm_fields = {
        "redis": (
            integer_matches(jvm_powershell, r"^\s*RedisMinimum\s*=\s*(\d+)$"),
            integer_matches(jvm_powershell, r"^\s*RedisMaximum\s*=\s*(\d+)$"),
        ),
        "application": (
            integer_matches(jvm_powershell, r"^\s*ApplicationMinimum\s*=\s*(\d+)$"),
            integer_matches(jvm_powershell, r"^\s*ApplicationMaximum\s*=\s*(\d+)$"),
        ),
    }
    for purpose, (minimums, maximums) in jvm_fields.items():
        actual = list(zip(minimums, maximums))
        expected = [
            resolved_ranges[(language, "sample", purpose)]
            for language in ("java", "kotlin")
        ]
        if actual != expected:
            raise ValueError(
                f"{jvm_powershell.relative_to(ROOT)}: mirrored JVM {purpose} "
                f"ranges are {actual}, expected {expected}"
            )


def verify_gradle_calls_locked(path: Path, source: str) -> None:
    lines = source.splitlines()
    for index, line in enumerate(lines):
        if "gradlew" not in line:
            continue
        context = "\n".join(lines[max(0, index - 8):index + 1])
        if "zlink_e2e_gradle_build_locked" not in context:
            raise ValueError(
                f"{path.relative_to(ROOT)}:{index + 1}: Gradle invocation is "
                "outside the shared Java/Kotlin build-only lock"
            )


def verify_explicit_bash_runner_launches(path: Path, source: str) -> None:
    """Reject shell-runner launches that depend on the executable mode bit."""
    collapsed = re.sub(r"\\\s*\n\s*", " ", source)
    command_prefix = (
        r"^\s*(?:(?:if|elif|while|until)\s+)?(?:!\s+)?(?:exec\s+)?"
        r"(?:[A-Za-z_][A-Za-z0-9_]*=(?:\"[^\"]*\"|'[^']*'|[^\s;]+)\s+)*"
        r"(?:timeout\s+(?:\"[^\"]*\"|'[^']*'|[^\s;]+)\s+)?"
        r"(?:env\s+(?:[A-Za-z_][A-Za-z0-9_]*="
        r"(?:\"[^\"]*\"|'[^']*'|[^\s;]+)\s+)*)?"
    )
    runner_target = (
        r"(?:\"\$0\"|\"\$\{BASH_SOURCE\[0\]\}\"|"
        r"\"\$\{(?:SCRIPT_PATH|LEGACY_RUNNER)\}\"|"
        r"\"[^\"\n]*run_(?:e2e|sample)\.sh\"|"
        r"'[^'\n]*run_(?:e2e|sample)\.sh'|"
        r"[^\s;]*run_(?:e2e|sample)\.sh)"
    )
    direct_launch = re.compile(command_prefix + runner_target + r"(?=\s|;|$)")
    for line_number, line in enumerate(collapsed.splitlines(), start=1):
        if direct_launch.search(line):
            raise ValueError(
                f"{path.relative_to(ROOT)}:{line_number}: direct shell-runner "
                "launch must use explicit bash"
            )


def verify_no_unowned_redis_operations(path: Path, source: str) -> None:
    forbidden = (
        (r"(?:127\.0\.0\.1|localhost):(?::)?6379\b",
         "default Redis port 6379 bypasses per-run ownership"),
        (r"ZLINK_TEST_REDIS_ENDPOINT",
         "external Redis endpoint fallback bypasses per-run ownership"),
        (r"\$\{[A-Za-z0-9_]*REDIS[A-Za-z0-9_]*ENDPOINT(?::-|-)",
         "Redis endpoint environment fallback bypasses per-run ownership"),
        (r"\$env:[A-Za-z0-9_]*REDIS[A-Za-z0-9_]*ENDPOINT\b",
         "PowerShell Redis endpoint fallback bypasses per-run ownership"),
        (r"process\.env\.[A-Za-z0-9_]*REDIS[A-Za-z0-9_]*ENDPOINT\b",
         "Node Redis endpoint fallback bypasses per-run ownership"),
    )
    for pattern, description in forbidden:
        if re.search(pattern, source, re.MULTILINE | re.IGNORECASE):
            raise ValueError(f"{path.relative_to(ROOT)}: {description}")

    collapsed = re.sub(r"\\\s*\n\s*", " ", source)
    if re.search(r"\bdocker(?:\.exe)?\s+(?:rm|container\s+rm)\b", collapsed,
                 re.IGNORECASE):
        raise ValueError(
            f"{path.relative_to(ROOT)}: Redis cleanup must use the shared "
            "exact-ID helper"
        )
    for line in collapsed.splitlines():
        if re.search(
            r"\bredis-cli\b.*\b(?:DEL|UNLINK|FLUSHDB|FLUSHALL)\b",
            line,
            re.IGNORECASE,
        ):
            raise ValueError(
                f"{path.relative_to(ROOT)}: broad Redis key cleanup must not "
                "run from a sample runner"
            )
        if re.search(
            r"\bdocker(?:\.exe)?\s+ps\b.*(?:--filter\s+)?name=",
            line,
            re.IGNORECASE,
        ):
            raise ValueError(
                f"{path.relative_to(ROOT)}: container name/prefix lookup must "
                "not be used for cleanup"
            )


def verify_sample_runner_helper(
    inventory: SampleRunnerInventory,
    path: Path,
    source: str,
) -> None:
    if path.suffix == ".sh":
        required = {
            "cpp": ("redis-common.sh", "zlink_redis_start_scoped_assign"),
            "dotnet": ("redis-common.sh", "zlink_redis_start_scoped_assign"),
            "java": ("runner-common.sh", "zlink_redis_start_scoped_assign"),
            "kotlin": ("runner-common.sh", "zlink_redis_start_scoped_assign"),
            "node": ("run-sample.mjs", "Runner/sample-runner.mjs"),
        }[inventory.language]
    elif inventory.language == "node":
        required = ("run-sample.mjs", "Runner/sample-runner.mjs")
    elif inventory.language == "dotnet" and "run_sample.sh" in source:
        required = ("Get-Command bash", "run_sample.sh")
    elif inventory.language == "dotnet":
        required = ("sample_runner.ps1", "Start-SampleRedisContainer")
    else:
        required = ("redis-common.ps1", "Start-ZlinkSampleRedis")
    for marker in required:
        if marker not in source:
            raise ValueError(
                f"{path.relative_to(ROOT)}: sample runner omits shared "
                f"lifecycle helper marker {marker!r}"
            )


def verify_sample_runner_inventories() -> tuple[int, list[Path]]:
    total = 0
    bash_runners: list[Path] = []
    fixed_runner_log = (
        r"(?im)^\s*[A-Za-z_][A-Za-z0-9_]*LOG[A-Za-z0-9_]*\s*=\s*"
        r"[\"']?\$\{?(?:SCRIPT_DIR|ROOT_DIR)\}?[/\\]"
        r"(?:logs|sample-logs|flow-logs)(?:[/\\]|[\"']|$)"
    )
    fixed_powershell_log = (
        r"(?im)^\s*\$[A-Za-z_][A-Za-z0-9_]*Log[A-Za-z0-9_]*\s*=\s*"
        r"Join-Path\s+\$(?:ScriptDir|SampleDir|PSScriptRoot)\s+"
        r"[\"'](?:logs|sample-logs|flow-logs)[\"']"
    )

    for inventory in SAMPLE_RUNNER_INVENTORIES:
        root = ROOT / inventory.root
        expected = []
        for sample in SAMPLE_NAMES:
            directory = (
                sample
                if inventory.language == "node" and sample == "ZoneWorld"
                else f"{sample}{inventory.directory_suffix}"
            )
            expected.append(f"{directory}/run_sample.sh")
            if sample in inventory.powershell_samples:
                expected.append(f"{directory}/run_sample.ps1")
        discovered = sorted(
            path.relative_to(root).as_posix()
            for path in root.glob("*/run_sample.*")
            if path.name in ("run_sample.sh", "run_sample.ps1")
        )
        expected = sorted(expected)
        if discovered != expected:
            missing = sorted(set(expected) - set(discovered))
            unexpected = sorted(set(discovered) - set(expected))
            raise ValueError(
                f"{inventory.root}: {inventory.language} sample runner "
                f"inventory changed; missing={missing}, unexpected={unexpected}"
            )

        for relative in expected:
            path = root / relative
            source = path.read_text(encoding="utf-8")
            if path.suffix == ".sh":
                if not source.startswith("#!/usr/bin/env bash\n"):
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: sample runner must declare Bash"
                    )
                verify_explicit_bash_runner_launches(path, source)
                bash_runners.append(path)
            verify_sample_runner_helper(inventory, path, source)
            verify_no_unowned_redis_operations(path, source)
            if inventory.language == "cpp" and (
                re.search(fixed_runner_log, source)
                or re.search(fixed_powershell_log, source)
            ):
                raise ValueError(
                    f"{path.relative_to(ROOT)}: sample logs must use a per-run "
                    "temporary directory"
                )
            if inventory.language == "cpp" and "FLOW_LOG_DIR" in source:
                assignments = re.findall(
                    r'(?m)^FLOW_LOG_DIR="([^"]+)"$', source
                )
                if assignments != ["$RUN_DIR/flow-logs"]:
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: C++ flow logs must be owned "
                        f"by RUN_DIR, found {assignments}"
                    )
                require_text(path, 'rm -rf "$RUN_DIR"')
            total += 1
    return total, bash_runners


def verify_runner_inventories() -> int:
    resource_markers = (
        "RUN_ID=", "run_id=", "mktemp", "mkdir -p",
        "zlink_cpp_e2e_allocate_", "zlink_dotnet_e2e_allocate_ports",
        "zlink_e2e_reserve_ports", "zlink_e2e_reserve_mixed_endpoints",
        "allocate_port", "pick_port", "zlink_redis_start_scoped",
        "start_redis_container", "docker create", "docker start",
    )
    forbidden_endpoint = r"127\.0\.0\.1:(?::)?6379\b"
    forbidden_dynamic_bind = (
        'bind(("127.0.0.1", 0))',
        's.bind((\'127.0.0.1\', 0))',
        's.bind((\'127.0.0.1\',0))',
    )
    total = 0
    for inventory in RUNNER_INVENTORIES:
        root = ROOT / inventory.root
        discovered = sorted(
            path.relative_to(root).as_posix()
            for path in root.rglob("run*.sh")
            if path.name == "run_e2e.sh"
            or re.fullmatch(r"run_.+_e2e\.sh", path.name)
            or path.name == "run_rm_a7_global_identity.sh"
        )
        expected = sorted(inventory.expected)
        if discovered != expected:
            missing = sorted(set(expected) - set(discovered))
            unexpected = sorted(set(discovered) - set(expected))
            raise ValueError(
                f"{inventory.root}: {inventory.language} E2E runner inventory "
                f"changed; missing={missing}, unexpected={unexpected}"
            )

        for relative in expected:
            path = root / relative
            source = path.read_text(encoding="utf-8")
            if not source.startswith("#!/usr/bin/env bash\n"):
                raise ValueError(
                    f"{path.relative_to(ROOT)}: E2E runner must declare Bash"
                )
            verify_explicit_bash_runner_launches(path, source)
            lock_count = source.count(inventory.lock_marker)
            if lock_count != 1:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: expected one whole-run lock "
                    f"call {inventory.lock_marker!r}, found {lock_count}"
                )
            lock_offset = source.index(inventory.lock_marker)
            for marker in resource_markers:
                resource_offset = source.find(marker)
                if 0 <= resource_offset < lock_offset:
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: acquires the whole-run lock "
                        f"after owned-resource marker {marker!r}"
                    )
            if inventory.language == "cpp":
                cleanup_marker = "zlink_cpp_e2e_install_cleanup_trap"
                cleanup_offset = source.find(cleanup_marker)
                if cleanup_offset < lock_offset:
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: missing early owned-resource cleanup"
                    )
                for marker in resource_markers:
                    resource_offset = source.find(marker)
                    if 0 <= resource_offset < cleanup_offset:
                        raise ValueError(
                            f"{path.relative_to(ROOT)}: installs owned-resource "
                            f"cleanup after {marker!r}"
                        )
            if re.search(forbidden_endpoint, source):
                raise ValueError(
                    f"{path.relative_to(ROOT)}: fixed or dynamic default Redis "
                    "port 6379 bypasses the scoped helper"
                )
            if "ZLINK_TEST_REDIS_ENDPOINT" in source:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: external Redis endpoint fallback "
                    "bypasses per-run ownership"
                )
            if re.search(
                r"\$\{ZLINK_[A-Z0-9_]*REDIS[A-Z0-9_]*ENDPOINT(?::-|-)",
                source,
            ):
                raise ValueError(
                    f"{path.relative_to(ROOT)}: Redis endpoint environment "
                    "fallback bypasses per-run ownership"
                )
            for match in re.finditer(
                r"[\"']keyPrefix[\"']\s*:\s*[\"']([^\"']+)[\"']",
                source,
            ):
                if not re.search(r"[$%{}]", match.group(1)):
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: literal JSON keyPrefix "
                        f"{match.group(1)!r} is shared across runs"
                    )
            for token in forbidden_dynamic_bind:
                if token in source:
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: dynamic bind(0) bypasses the "
                        "language application-port pool"
                    )
            if re.search(r"\bdocker\s+(?:rm|container\s+rm)\b", source):
                raise ValueError(
                    f"{path.relative_to(ROOT)}: Redis cleanup must use the "
                    "shared exact-ID helper"
                )
            collapsed_source = re.sub(r"\\\s*\n\s*", " ", source)
            for line in collapsed_source.splitlines():
                if re.search(
                    r"\bredis-cli\b.*\b(?:DEL|UNLINK|FLUSHDB|FLUSHALL)\b",
                    line,
                    re.IGNORECASE,
                ):
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: broad Redis key cleanup must "
                        "not run from an E2E runner"
                    )
                if re.search(
                    r"\bdocker\s+ps\b.*(?:--filter\s+)?name=",
                    line,
                    re.IGNORECASE,
                ):
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: container name/prefix lookup "
                        "must not be used for cleanup"
                    )
            if inventory.language in ("java", "kotlin"):
                verify_gradle_calls_locked(path, source)
            total += 1
    return total


def verify_aggregate_runners() -> None:
    lock_tokens = {
        "cpp": ("zlink_cpp_e2e_acquire_run_lock",),
        "dotnet": ("zlink_dotnet_e2e_acquire_run_lock",),
        "java": ("zlink_e2e_initialize", "e2e-runner-common.sh"),
        "kotlin": ("zlink_e2e_initialize", "e2e-runner-common.sh"),
        "node": ("serialize_node_e2e_run", "NODE_E2E_LANGUAGE_LOCK_FILE"),
    }
    for aggregate in AGGREGATE_RUNNERS:
        path = ROOT / aggregate.path
        source = path.read_text(encoding="utf-8")
        if not source.startswith("#!/usr/bin/env bash\n"):
            raise ValueError(f"{path.relative_to(ROOT)}: aggregate must declare Bash")
        for token in (*lock_tokens[aggregate.language], "flock"):
            if token in source:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: aggregate must leave the "
                    f"whole-run lock to each child; found {token!r}"
                )
        if "bash ./run_e2e.sh" not in source:
            raise ValueError(
                f"{path.relative_to(ROOT)}: child runners must be invoked with Bash"
            )
        if re.search(r"\[\[[^\n]*!\s+-x[^\n]*run_e2e\.sh", source):
            raise ValueError(
                f"{path.relative_to(ROOT)}: aggregate must accept 0644 child runners"
            )
        call_lines = [
            line.strip()
            for line in source.splitlines()
            if aggregate.child_call in line
        ]
        if not call_lines:
            raise ValueError(
                f"{path.relative_to(ROOT)}: missing sequential aggregate call "
                f"{aggregate.child_call!r}"
            )
        if any(re.search(r"(?<!&)&\s*$", line) for line in call_lines):
            raise ValueError(
                f"{path.relative_to(ROOT)}: aggregate configuration calls must "
                "run in the foreground"
            )

        child_launch = source.find("bash ./run_e2e.sh")
        if aggregate.active_pid_marker is not None:
            active_pid = source.find(aggregate.active_pid_marker, child_launch)
            wait = source.find(aggregate.wait_marker or "", active_pid)
            if not (child_launch >= 0 and active_pid > child_launch and wait > active_pid):
                raise ValueError(
                    f"{path.relative_to(ROOT)}: must wait for each launched child "
                    "before starting another configuration"
                )
        elif re.search(r"(?m)(?<!&)&\s*$", source):
            raise ValueError(
                f"{path.relative_to(ROOT)}: synchronous aggregate unexpectedly "
                "backgrounds a command"
            )


def verify_lock_reentrancy() -> None:
    helper_contracts = {
        "framework/languages/cpp/e2e/redis-common.sh": (
            "zlink_cpp_e2e_run_lock_held", "return 0",
            "exec flock --exclusive --close", 'bash "${runner}" "$@"',
        ),
        "framework/languages/dotnet/e2e/redis-common.sh": (
            "ZLINK_DOTNET_E2E_RUN_LOCK_HELD", "return 0",
            "exec flock --close", 'bash "${runner}" "$@"',
        ),
        "framework/languages/java/e2e-runner-common.sh": (
            "ZLINK_JAVA_E2E_RUN_LOCK_HELD",
            "ZLINK_KOTLIN_E2E_RUN_LOCK_HELD", "return 0",
            "exec flock --exclusive --close", 'bash "${runner}" "$@"',
        ),
        "framework/languages/node/e2e/runner-common.sh": (
            "ZLINK_NODE_E2E_LANGUAGE_LOCK_HELD", "return 0",
            "exec flock --exclusive --close", 'bash "$runner_path" "$@"',
        ),
    }
    for relative, required in helper_contracts.items():
        path = ROOT / relative
        source = path.read_text(encoding="utf-8")
        if "TMPDIR" in source:
            raise ValueError(
                f"{path.relative_to(ROOT)}: whole-run lock must use fixed /tmp"
            )
        for text in required:
            if text not in source:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: reentrant whole-run lock is "
                    f"missing {text!r}"
                )


def verify_jvm_build_lock() -> None:
    lock_path = "/tmp/zlink-framework-java-kotlin-sample-gradle.lock"
    e2e_helper = ROOT / "framework/languages/java/e2e-runner-common.sh"
    sample_helper = ROOT / "framework/languages/java/samples/runner-common.sh"
    for path in (e2e_helper, sample_helper):
        require_text(path, lock_path)
    require_text(
        ROOT / "framework/languages/java/samples/redis-common.ps1",
        "zlink-framework-java-kotlin-sample-gradle.lock",
    )
    build_function = shell_function(e2e_helper, "zlink_e2e_gradle_build_locked")
    if 'flock --exclusive --close "${lock_path}" "$@"' not in build_function:
        raise ValueError(
            f"{e2e_helper.relative_to(ROOT)}: JVM build lock must wrap only the "
            "requested build command and close inherited descriptors"
        )
    if "exec flock" in build_function:
        raise ValueError(
            f"{e2e_helper.relative_to(ROOT)}: JVM build lock must be released "
            "when the build command returns"
        )


def verify_shell_redis_helpers() -> int:
    for helper in SHELL_REDIS_HELPERS:
        path = ROOT / helper.path
        source = path.read_text(encoding="utf-8")
        start = shell_function(path, helper.start_function)
        attempt = shell_function(path, helper.attempt_function)
        cleanup = shell_function(path, helper.cleanup_function)
        collapsed_start = re.sub(r"\\\s*\n\s*", " ", start)
        collapsed_cleanup = re.sub(r"\\\s*\n\s*", " ", cleanup)

        for command in ("create", "start"):
            occurrences = list(re.finditer(rf"\bdocker\s+{command}\b", collapsed_start))
            if not occurrences:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: Redis helper does not run docker {command}"
                )
            for occurrence in occurrences:
                line_start = collapsed_start.rfind("\n", 0, occurrence.start()) + 1
                if "timeout" not in collapsed_start[line_start:occurrence.start()]:
                    raise ValueError(
                        f"{path.relative_to(ROOT)}: docker {command} is not bounded"
                    )
        for marker in (
            ".State.Running", ".NetworkSettings.Ports", "6379/tcp",
        ):
            if marker not in start:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: Redis start verification is "
                    f"missing {marker!r}"
                )
        if not re.search(r"running[^\n]*(?:==|!=)[^\n]*true", start, re.IGNORECASE):
            raise ValueError(
                f"{path.relative_to(ROOT)}: Redis helper does not verify running state"
            )
        if not re.search(
            r"(?:host_port|published_port|selected_port)[^\n]*(?:==|!=)[^\n]*"
            r"(?:port|host_port|selected_port)",
            start,
        ):
            raise ValueError(
                f"{path.relative_to(ROOT)}: Redis helper does not compare the "
                "published port with the selected port"
            )
        if "127.0.0.1::6379" in source:
            raise ValueError(
                f"{path.relative_to(ROOT)}: dynamic Docker Redis port bypasses "
                "the language pool"
            )
        if "docker inspect --type container" not in re.sub(
            r"\\\s*\n\s*", " ", attempt
        ):
            raise ValueError(
                f"{path.relative_to(ROOT)}: failed create cleanup cannot resolve "
                "the exact container ID from its unique attempt name"
            )
        if "^[0-9a-f]{12,64}$" not in attempt + cleanup:
            raise ValueError(
                f"{path.relative_to(ROOT)}: Redis cleanup does not validate an exact ID"
            )
        rm_occurrences = list(re.finditer(r"\bdocker\s+rm\s+-fv\b", collapsed_cleanup))
        if not rm_occurrences:
            raise ValueError(
                f"{path.relative_to(ROOT)}: exact-ID Redis cleanup is missing"
            )
        for occurrence in rm_occurrences:
            line_start = collapsed_cleanup.rfind("\n", 0, occurrence.start()) + 1
            if "timeout" not in collapsed_cleanup[line_start:occurrence.start()]:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: docker rm cleanup is not bounded"
                )
    return len(SHELL_REDIS_HELPERS)


def verify_non_shell_sample_redis_helpers() -> None:
    powershell_helpers = (
        ROOT / "framework/languages/cpp/samples/redis-common.ps1",
        ROOT / "framework/languages/dotnet/samples/sample_runner.ps1",
        ROOT / "framework/languages/java/samples/redis-common.ps1",
    )
    required = (
        "WaitForExit($TimeoutSeconds * 1000)", "Kill($true)",
        '"create"', '"start"', "{{.State.Running}}",
        ".NetworkSettings.Ports", "^[0-9a-f]{12,64}$", '"rm", "-fv"',
    )
    for path in powershell_helpers:
        source = path.read_text(encoding="utf-8")
        for text in required:
            if text not in source:
                raise ValueError(
                    f"{path.relative_to(ROOT)}: bounded exact-ID Redis lifecycle "
                    f"is missing {text!r}"
                )
        if "127.0.0.1::6379" in source:
            raise ValueError(
                f"{path.relative_to(ROOT)}: dynamic Docker Redis port bypasses "
                "the language pool"
            )

    node_helper = ROOT / "framework/languages/node/samples/run-sample.mjs"
    for text in (
        "dockerCommandTimeoutMs", "timeout: dockerCommandTimeoutMs",
        "'create'", "'start'", "{{.State.Running}}",
        ".NetworkSettings.Ports", "/^[0-9a-f]{12,64}$/",
        "'inspect', '--type', 'container'", "['rm', '-fv', exactId]",
    ):
        require_text(node_helper, text)
    require_absent(
        node_helper,
        r"127\.0\.0\.1::6379",
        "dynamic Docker Redis port bypasses the language pool",
    )


def main() -> int:
    if sys.argv[1:] not in ([], ["--check"]):
        raise ValueError("usage: verify-framework-runner-isolation.py [--check]")
    resolved: list[tuple[RangeSource, int, int]] = []
    language_order = ("cpp", "dotnet", "java", "kotlin", "node")
    for source in RANGES:
        path = ROOT / source.path
        minimum = int(exact_match(path, source.minimum_pattern))
        maximum = int(exact_match(path, source.maximum_pattern))
        if not 1 <= minimum <= maximum <= 65535:
            raise ValueError(
                f"{source.language} {source.suite} {source.purpose}: "
                f"invalid range {minimum}-{maximum}"
            )
        suite_minimum, suite_maximum = (
            (20000, 29999) if source.suite == "sample" else (30000, 39999)
        )
        if minimum < suite_minimum or maximum > suite_maximum:
            raise ValueError(
                f"{source.language} {source.suite} {source.purpose}: "
                f"{minimum}-{maximum} is outside {suite_minimum}-{suite_maximum}"
            )
        language_base = suite_minimum + language_order.index(source.language) * 2000
        expected = (
            (language_base, language_base + 99)
            if source.purpose == "redis"
            else (language_base + 100, language_base + 1999)
        )
        if (minimum, maximum) != expected:
            raise ValueError(
                f"{source.language} {source.suite} {source.purpose}: "
                f"expected {expected[0]}-{expected[1]}, got {minimum}-{maximum}"
            )
        resolved.append((source, minimum, maximum))

    for left, right in itertools.combinations(resolved, 2):
        left_source, left_minimum, left_maximum = left
        right_source, right_minimum, right_maximum = right
        if max(left_minimum, right_minimum) <= min(left_maximum, right_maximum):
            raise ValueError(
                "runner port ranges overlap: "
                f"{left_source.language}/{left_source.suite}/{left_source.purpose} "
                f"{left_minimum}-{left_maximum} and "
                f"{right_source.language}/{right_source.suite}/{right_source.purpose} "
                f"{right_minimum}-{right_maximum}"
            )

    by_suite_language = {
        (source.suite, source.language): {
            purpose: (minimum, maximum)
            for current_source, minimum, maximum in resolved
            if current_source.suite == source.suite
            and current_source.language == source.language
            for purpose in (current_source.purpose,)
        }
        for source, _, _ in resolved
    }
    for (suite, language), purposes in by_suite_language.items():
        redis_minimum, redis_maximum = purposes["redis"]
        app_minimum, app_maximum = purposes["application"]
        row = (
            f'| {LANGUAGE_LABELS[language]} | '
            f'`{redis_minimum}-{redis_maximum}` | '
            f'`{app_minimum}-{app_maximum}` |'
        )
        for readme_path in README_PATHS[suite]:
            require_once(ROOT / readme_path, row)

    for helper_path, runner_path in TEMPLATE_PAIRS:
        helper = ROOT / helper_path
        runner = ROOT / runner_path
        require_once(helper, "zlink_redis_remove_by_id() {")
        require_once(
            helper,
            '[[ "${container_id}" =~ ^[0-9a-f]{12,64}$ ]] || return 1',
        )
        require_once(
            runner,
            'zlink_redis_remove_by_id "${REDIS_CONTAINER_ID}" || true',
        )
        if "docker rm" in runner.read_text(encoding="utf-8"):
            raise ValueError(
                f"{runner.relative_to(ROOT)}: cleanup must use the shared helper"
            )

    lock_paths = {
        source.language: exact_match(ROOT / source.path, source.pattern)
        for source in LOCKS
    }
    if len(set(lock_paths.values())) != len(lock_paths):
        raise ValueError(f"E2E language lock paths are not unique: {lock_paths}")

    resolved_ranges = {
        (source.language, source.suite, source.purpose): (minimum, maximum)
        for source, minimum, maximum in resolved
    }
    verify_mirrored_ranges(resolved_ranges)
    sample_runner_count, _ = verify_sample_runner_inventories()
    runner_count = verify_runner_inventories()
    verify_aggregate_runners()
    verify_lock_reentrancy()
    verify_jvm_build_lock()
    redis_helper_count = verify_shell_redis_helpers()
    verify_non_shell_sample_redis_helpers()

    print(
        "FRAMEWORK RUNNER ISOLATION CLEAN "
        f"ranges={len(resolved)} locks={len(lock_paths)} "
        f"runners={runner_count} sample_runners={sample_runner_count} "
        f"redis_helpers={redis_helper_count}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"FRAMEWORK RUNNER ISOLATION FAILED: {error}", file=sys.stderr)
        raise SystemExit(1)
