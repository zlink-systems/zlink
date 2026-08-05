# Stream Compare Benchmark

## Purpose

`benchwithstreamcompare` compares stream echo performance between `zlink` and
other stream stacks under one unified benchmark flow.

Primary goals:

- Compare library behavior with one common client implementation.
- Keep the benchmark close to pure stream socket echo.
- Avoid cross-run interference by running one benchmark process at a time.
- Measure throughput and latency in one pass per case.

## What Is Included

- `run_benchmarks.sh`: end-to-end benchmark runner.
- `client/bench_stream_client.cpp`: common benchmark client.
- `stacks/*`: per-stack server sources and project files.
- `run_comparison.py`: summary and ranking generator.

Supported stacks:

- `asio`
- `cppserver`
- `dotnet`
- `netzlink`
- `jvmzlink`
- `zlink`
- `zmq`
- `netty`

Supported payload sizes:

- `64`
- `1024`
- `65536`

## Design Notes (Fairness)

- All stacks run the framed echo path in their default server configuration.
- The same client binary is used for all stacks.
- The client always uses one wire format for every stack:
  `4-byte big-endian payload length + payload`.
- Runner uses one server process per stack and executes stacks sequentially.
- For multi-size runs, runner reconnects per size (server restart + client reconnect).
- Run order is size-first: for each selected size, runner executes all runs
  (`size -> run -> stack`).
- Size summary output is generated once after all runs for that size complete.

## Requirements

- Linux environment
- CMake + C++ toolchain
- Python 3
- .NET SDK (for `dotnet`, `netzlink`, `netzlink-len32be` stacks)
- JDK 22 + Gradle 8.8+ (for `netty`, `jvmzlink` stacks)
- Optional external libs/tools depending on selected stacks

For high CCU (for example `--ccu 10000`), host tuning is important:

- file descriptor limit (`ulimit -n`)
- local ephemeral port range (`net.ipv4.ip_local_port_range`)
- backlog and TCP memory settings (OS dependent)

Example checks:

```bash
ulimit -n
cat /proc/sys/net/ipv4/ip_local_port_range
```

## Quick Start

From repository root:

```bash
./bindings/c/bench/with_stream/run_benchmarks.sh
```

Run selected stacks and size:

```bash
./bindings/c/bench/with_stream/run_benchmarks.sh \
  --stack zlink,zmq,dotnet \
  --size 65536 \
  --ccu 10000 \
  --runs 3 \
  --warmup 3 \
  --duration 5
```

Run one stack with multi-size sequence (reconnect per size):

```bash
./bindings/c/bench/with_stream/run_benchmarks.sh \
  --stack zlink \
  --size 64,1024,65536
```

## Runner Options

```text
--stack <asio|cppserver|dotnet|netzlink|jvmzlink|zlink|zmq|netty|all|csv>
--size <64|1024|65536|all|csv>
--build-dir PATH            default: core/build
--reuse-build               reuse existing build directory (skip configure/build)
--clean-build               remove build directory and do a clean build
--ccu <N>                    default: 1000
--runs <N>                   default: 1 (repeated per size)
--warmup <sec>               default: 3
--duration <sec>             default: 5
--client-io-threads <N>      default: 4
--server-io-threads <N>      default: 4
--resource-sample-ms <N>     default: 500
--server-start-timeout <sec> default: 40
--stack-gap <sec>            default: 5
```

Supported environment variables:

- `RESULT_DIR`: override output directory
- `HOST`: benchmark target host (default `127.0.0.1`)
- `BASE_PORT`: start port for stack runs (default `22000`)
- `NETTY_JAVA_HOME`: JDK home for `netty`, `jvmzlink` stacks (must be Java 22+)
- `NETTY_GRADLE_BIN`: override Gradle binary for `netty`, `jvmzlink` stacks

Notes:

- A lock file (`/tmp/bench_streamcompare.lock`) blocks concurrent benchmark runs.
- Build mode default is incremental. `--reuse-build` and `--clean-build` are mutually exclusive.
- `--reuse-build` requires an existing build directory and existing binaries for selected stacks.
- `--clean-build` removes core build directory and stack-local build outputs before rebuilding.
- Stacks are built before execution. Build failure for one stack is recorded as
  skipped, not a hard stop for all stacks.
- `netty` requires Java 22+ and resolves Java in this order:
  `NETTY_JAVA_HOME -> JAVA_HOME -> PATH java`.
- `netty` requires Gradle 8.8+. If system `gradle` is older, the runner
  auto-downloads Gradle `8.10.2` under
  `bindings/c/bench/with_stream/stacks/netty/.gradle-tools/`.
- `jvmzlink` uses the same Java/Gradle resolution path as `netty`, then builds
  `bindings/java` jar before packaging the stack server app.
- `zlink` stack is run directly as the native STREAM server binary.

## Output Files

Default output directory:

- `bindings/c/bench/with_stream/results/<timestamp>/`

Generated files:

- `metrics.csv`: raw per-case metrics
- `summary.json`: summarized stats
- `comparison.md`: human-readable report
- `skipped_stacks.csv`: skipped stacks and reasons
- `logs/*_client.log`, `logs/*_server.log`: per-stack logs
- `logs/*_client_resource.csv`, `logs/*_server_resource.csv`: sampled process usage
- `logs/*_system_resource.csv`: sampled host-level usage during each stack run

`metrics.csv` key fields:

- `throughput_bps`, `throughput_mib_s`
- `p50_us`, `p95_us`, `p99_us`
- `connect_ok`, `connect_fail`
- `send_err`, `recv_err`, `timeout`
- `pass_fail`
- `client_avg_cpu_pct`, `client_peak_cpu_pct`
- `client_avg_rss_kb`, `client_peak_rss_kb`, `client_peak_hwm_kb`
- `server_avg_cpu_pct`, `server_peak_cpu_pct`
- `server_avg_rss_kb`, `server_peak_rss_kb`, `server_peak_hwm_kb`
- `system_avg_cpu_pct`, `system_peak_cpu_pct`
- `system_avg_mem_used_kb`, `system_peak_mem_used_kb`
- `system_avg_mem_used_pct`, `system_peak_mem_used_pct`

Important interpretation note:

- Throughput and latency are measured from the same single client run per
  `(stack, size, run)`. The report still keeps throughput/latency sections for
  compatibility, but both sections come from the same measurement rows.

## Pass/Fail Meaning

Each case is marked `PASS` only when:

- connections are established successfully
- no send/recv/timeout errors
- positive throughput is measured

## Size Contamination Check

Recommended check flow:

1. Run one multi-size benchmark in a single run.
2. Run each size separately under the same options.
3. Compare throughput deltas and error counters.
4. If needed, increase `--runs` and compare median values.

## Known Limits

- Benchmark runner keeps compatibility `phase` labels in outputs, while actual
  collection is single-pass.
