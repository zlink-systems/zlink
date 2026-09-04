# bindings/c/perf

This directory contains the standalone C benchmark runner and comparison tools.

## Core Runtime Rule

The Unix shell runners use the current workspace `core/build` runtime by
default. Pass `--core-version MAJOR.MINOR.PATCH` to download and verify that
released Core instead. The Windows PowerShell runners select the same local
mode with `ZLINK_CORE_SOURCE=local`; their default remains the verified release
package. A downloaded release prefix is cached by
`scripts/local-package/core/fetch-release.sh` or
`scripts/local-package/core/fetch-release.ps1`.

```bash
./scripts/local-package/core/fetch-release.sh --version 0.13.1
./bindings/c/perf/run_benchmarks_multi.sh --pattern ROUTER_ROUTER_REQREP
```

Before running benchmarks, the script prints the resolved `libzlink.so` path.
Local mode rebuilds `core/build` when required and rejects a runtime older than
`core/src` or `core/include`. Explicit release mode disables source mtime checks
because it verifies and uses the downloaded package instead.

## CI Multi Smoke

Run `./bindings/c/perf/ci_multi_smoke.sh` for the bounded TCP multi-socket CI
matrix; it defaults to 8 clients, a 2-second active window, message sizes
`4096,65536`, and patterns `DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB`, and
fails if any pattern/size throughput `RESULT` is missing or the 300-second
deadline expires. Override those values with `CCU`, `DUR`, `SIZES`, `PATTERNS`,
and `TIMEOUT` (or the corresponding `--ccu`, `--dur`, `--sizes`, `--patterns`,
and `--timeout` arguments), for example:

```bash
CCU=4 DUR=1 SIZES=4096 PATTERNS=DEALER_DEALER TIMEOUT=60 \
  ./bindings/c/perf/ci_multi_smoke.sh
```

## Cross-Binding Handshake Reference

`bindings/c/perf` is the canonical benchmark handshake for all binding-language
perf runners. When C, C++, .NET, Java, Rust, Go, Node, or Python perf results
are compared, their runner/process orchestration must match the C perf
handshake before the numbers are treated as comparable.

The fixed reference surface is:

- runner and benchmark process stdin/stdout tokens (`READY`, `CLIENT_READY`,
  `CLIENT_DONE`, `START`, `PHASE_ACTIVE`, `PHASE_LATENCY`, `PHASE_DONE`,
  `LATENCY_READY`, `LATENCY_ACK`, `STOP`, `RESULT`);
- raw socket connection gates based on the same C perf `CONNECTION_READY`
  meaning, plus the C perf `CLIENT_READY` / `START` start barrier for the
  one-way raw patterns that use it;
- active window, stop/drain, timeout, fail, skip, and unsupported semantics;
- RESULT line metric names and anchor points.

`PHASE_ACTIVE,<msg_size>` is a C runner compatibility token for some one-way
paths. Benchmark processes must not require it as an extra active gate; the
required active gate is the pattern-specific C handshake documented in
`doc/perf`. Only patterns that use the C runner `CLIENT_READY` / `START`
barrier may require `START,<msg_size>`.

If a binding cannot implement this handshake through its public API, add the
missing public binding API or mark that perf combination `UNSUPPORTED`. Do not
use private binding members, reflection, raw native handles, sleeps, or
language-specific warmup gates to make a result line comparable.

Handshake changes are made in this order: update `bindings/c/perf` first, update
`doc/perf/PERF_POLICY.md` and the suite policy, then port the same contract to
other binding perf runners.

## Socket Sizing Policy

The default single and multi benchmark paths use context auto-HWM.
Both runners default the context auto-HWM profile to `balanced`. Pass
`--auto-hwm-profile compact`, `--auto-hwm-profile low_latency`,
`--auto-hwm-profile balanced`, or `--auto-hwm-profile throughput` to select a
different profile for a run. The same value can be supplied through
`PERF_SINGLE_CTX_AUTO_HWM_PROFILE`, `PERF_MULTI_CTX_AUTO_HWM_PROFILE`, or the
shared `PERF_CTX_AUTO_HWM_PROFILE`.
Runner defaults do not inject numeric `SNDHWM`, `RCVHWM`, `SNDBUF`, or
`RCVBUF` into benchmark sockets. If you need a manual override for debug,
set `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` or
`PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1` (or the shared
`PERF_ALLOW_MANUAL_SOCKET_OVERRIDES=1`) and then pass explicit
`PERF_SINGLE_*` / `PERF_MULTI_*` socket values. The runner option `--buf 128k`
is a shortcut that sets both send and receive socket buffers to the same value;
`--sndbuf` and `--rcvbuf` are still available when each direction needs a
different value.
Manual HWM values are bytes. The previous message-count meaning is not
accepted. A former 1,000-message setting maps to `4,096,000` bytes when the
4 KiB planning unit is used.

Multi benchmarks set the auto-HWM message unit from the current message size
before each run:

| Socket family | Message unit used by perf |
|---------------|---------------------------|
| raw multi perf sockets | `msg_size` |

A visible `MsgUnit(B)=4096` row is only valid for a 4096 byte test case.
If a 64 byte or 1024 byte run prints `4096`, the run is not a valid comparison
because the effective slot budget and socket buffers are different from the
target message size.

All single benchmarks use one context I/O thread by default. Pass
`--io-threads` or set `PERF_IO_THREADS` only when intentionally running a
non-baseline diagnostic.

The auto-HWM detail table is printed after the result rows. It uses the cached
runtime snapshots and includes the applied HWM and socket buffers.

OOM must not be avoided by lowering the official workload's send rate, payload
size, client count, active duration, or concurrency. Each message-size phase
updates the context auto-HWM message unit and recalculates auto-HWM. Normal
burst control belongs to auto-HWM and public-API backpressure. If memory keeps
growing without `EAGAIN`, or phase/process teardown does not release queued
messages, treat that behavior as a Core bug and rerun the same workload after
the fix.

The bounded reservoirs used for p95 and p99 protect measurement metadata only.
They do not change queue HWM, send timing, receive count, or mean-latency
aggregation.

## Core 10.x archive

Files already present under `baseline/` are a read-only Core 10.x archive.
Core 0.9.0 benchmark runs write only to `results/`; the runners neither update nor
select the archive as an active baseline.

## Auto-HWM Profile Sweep

Use `--auto-hwm-profile` or `PERF_CTX_AUTO_HWM_PROFILE` with benchmark message
sizes to exercise the per-connection auto-HWM policy.
The recommended sweep axes are:

| Axis | Values |
|------|--------|
| profile | `low_latency`, `balanced`, `throughput` |
| message sizes | `64`, `1024`, `4096`, `65536` bytes |
| patterns | `DEALER_ROUTER_SENDSEND`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_SENDSEND`, `ROUTER_ROUTER_REQREP`, `PUBSUB`, `STREAM` |

Profile guidance for byte-budget Auto HWM:

| Profile | Use case |
|---------|----------|
| `compact` | smallest queue depth and 128 KiB socket-buffer floor for memory-sensitive runs |
| `low_latency` | lower per-connection queue depth for small latency-focused tests |
| `balanced` | default profile for ordinary service messages |
| `throughput` | larger per-connection queue depth for high throughput sweeps |

If calculated HWM values are too small for the target traffic shape, first
select a larger profile or configure the context-wide Auto HWM byte budget.

## One-Way Measurement Phases

One-way patterns measure throughput during a saturated active phase, drain its
accepted records, and then measure latency for one second with one record in
flight per logical client (or one globally when the socket pattern cannot route
an acknowledgement back to a specific client). The latency `RESULT` fields
therefore exclude active-phase HWM queue residence time.

An ordinary `DONTWAIT` socket send makes one admission attempt. Immediate
admission returns completion ID zero and produces no completion. On
`BACKPRESSURED`/`EAGAIN`, Core returns a nonzero wait token but retains no
payload; the benchmark keeps one exact logical packet per socket, waits for
`POLLOUT`/`POLLCOMPLETION`, drains the completion queue to `NO_DATA`, matches a
`WRITABLE` record by token, context, and routed RID when present, and resubmits
the same packet. `ZLINK_OPT_PENDING_MAX_MSGS` and
`ZLINK_OPT_PENDING_MAX_BYTES` bound pending `REQUEST` records only and do not
govern these ordinary sends. PUB publish operations have no send completions;
PUBSUB instead uses its no-drop backpressure path and an explicit subscriber
acknowledgement between latency records.
