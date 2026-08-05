# 11 — Performance Testing

[← Packaging](10-packaging.en.md) | [Table Of Contents](INDEX.en.md)

---

`connector_perf_client` is a tool that verifies whether a large number of general clients can
concurrently perform a request/wait flow through the connector. Server orchestration is handled by a
runner script or CTest fixture, while the perf executable is responsible only for generating load and
writing the report.

---

## Build

```bash
cmake --build framework/languages/cpp/build --target connector_perf_client
```

---

## Smoke — CI Structural Regression

Smoke runs with 10 clients, 2 workers, and a 5-second loopback setting, with no external server.

```bash
cmake --build framework/languages/cpp/build --target connector_perf_smoke
ctest --test-dir framework/languages/cpp/build -L connector-perf-smoke --output-on-failure
```

Smoke confirms:
- The `co_await request().async()`, `co_await wait_for().async()` flows work.
- A JSON report is generated with the correct schema.
- The worker thread count is fixed, regardless of the client count.

---

## Scale — Measuring 5,000 Clients

A dedicated STREAM test server endpoint is needed.

```bash
framework/languages/cpp/build/connector_perf_client \
  --clients       5000 \
  --workers       4 \
  --duration      60s \
  --warmup        10s \
  --request-timeout-ms 1000 \
  --transport     tcp \
  --dispatch-mode immediate \
  --endpoint      tcp://game-test-server.internal:7000 \
  --report        /tmp/connector-perf-5000.json
```

### Options

| Option | Meaning | Default |
|------|------|--------|
| `--clients` | number of concurrent connectors | `5000` |
| `--workers` | number of Asio worker threads | `4` |
| `--duration` | measurement duration | `60s` |
| `--warmup` | warmup duration | `10s` |
| `--payload-bytes` | request payload size | `256` |
| `--inflight` | concurrent requests per client | `1` |
| `--wait-clients-percent` | ratio of clients that also register a push-notification wait | `10` |
| `--transport` | `tcp`, `tls`, `ws`, `wss` | `tcp` |
| `--dispatch-mode` | `manual`, `immediate` | `immediate` |
| `--request-timeout-ms` | request timeout | `1000` |
| `--endpoint` | server address | required (except for loopback smoke) |
| `--report` | JSON report path | auto-generated under the build dir |

`--workers` isn't a value recorded only in the report. The perf client sets the shared connector
runtime's worker thread count to this value before creating connectors.

---

## Report Format

```json
{
  "clients":          5000,
  "workers":          4,
  "duration_ms":      60000,
  "requests_total":   2847361,
  "throughput_rps":   47456,
  "latency_p50_us":   412,
  "latency_p95_us":   1823,
  "latency_p99_us":   4201,
  "timeouts_total":   3,
  "errors_total":     0,
  "rss_bytes":        184549376,
  "cpu_user_ms":      87432,
  "cpu_system_ms":    14211
}
```

---

## Regression Judgment Criteria

Because hardware differences are large, a failure isn't triggered by absolute numbers alone. The
following items are judged as a regression.

| Item | Regression Signal |
|------|-----------|
| thread count | threads increase proportionally to client count |
| blocking | a worker thread gets tied up in a blocking wait during an async request |
| latency | p99 gets worse than the same-environment baseline by more than a specified ratio |
| timeout/error | the timeout/error ratio exceeds the threshold |
| RSS | RSS grows abnormally relative to client count |

---

## Runner Script

```bash
framework/languages/cpp/connector/perf/run_connector_perf.sh \
  --clients 5000 \
  --workers 4 \
  --endpoint tcp://game-test-server.internal:7000
```

The runner script handles starting the server, running the perf client, and collecting the report.
Record the run's results, along with the execution environment, CPU, OS, compiler, and build type,
in the PR description or a performance report.

---

## CTest Labels

| Label | Content |
|-------|------|
| `connector-perf-smoke` | 10-client loopback smoke |
| `connector-perf-scale` | 5,000-client scale (dedicated environment) |

```bash
ctest --test-dir framework/languages/cpp/build \
  -L connector-perf-smoke \
  --output-on-failure
```
