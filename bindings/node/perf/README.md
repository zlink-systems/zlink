# Node Perf

Node perf exposes in-repo single and multi runners for the aligned binding
surface. The authoritative policy contract remains:

- [`bindings/README.md`](/home/hep7/project/kairos/zlink/doc/spec/bindings/README.md)
- [`doc/perf/PERF_POLICY.md`](/home/hep7/project/kairos/zlink/doc/perf/PERF_POLICY.md)
- [`doc/perf/PERF_SINGLE_TEST_POLICY.md`](/home/hep7/project/kairos/zlink/doc/perf/PERF_SINGLE_TEST_POLICY.md)
- [`doc/perf/PERF_MULTI_TEST_POLICY.md`](/home/hep7/project/kairos/zlink/doc/perf/PERF_MULTI_TEST_POLICY.md)

Available entrypoints:

- `./perf/run_benchmarks.sh`
- `./perf/run_benchmarks_multi.sh`
- `./perf/single/run_benchmarks.sh`
- `./perf/multi/run_benchmarks.sh`

Current implemented scope:

- single patterns:
  - `PAIR`
  - `PUBSUB`
  - `DEALER_DEALER`
  - `DEALER_ROUTER`
  - `ROUTER_ROUTER`
- multi patterns:
  - `MULTI_DEALER_DEALER`
  - `MULTI_DEALER_ROUTER`
  - `MULTI_ROUTER_ROUTER`
  - `MULTI_PUBSUB`
  - `MULTI_STREAM`

Current alignment notes:

- both runners are recv-only and follow the policy metric header / RESULT
  contract
- `MULTI_STREAM` uses the Node public API server with the shared core
  `perf_stream_client` client required by the multi-suite policy. The Node
  runner passes `--completion-wait-ms` to that client from
  `PERF_MULTI_STREAM_COMPLETION_WAIT_MS`, then `PERF_STREAM_COMPLETION_WAIT_MS`,
  and otherwise uses `2000` ms so in-flight replies after the active window are
  counted without the instability seen with longer waits.
- For `MULTI_STREAM` over `ws` and `wss`, the Node runner caps the shared
  stream client fanout at `1000` by default. Override it with
  `PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX` or `PERF_STREAM_NON_TCP_CLIENTS_MAX`
  when a run intentionally needs higher non-TCP concurrency.
- result files are written under the shared `perf/results/{single,multi}/report`
  layout required by policy
- benchmark code is split by pattern file, and the entry scripts select the
  pattern through `--pattern`

Result files are written under:

- `perf/results/single/report/`
- `perf/results/multi/report/`

The runners emit official-style `RESULT,current,...` lines and keep the pattern
hot path inside each benchmark file.
