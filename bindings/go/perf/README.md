# Go performance benchmarks

The single and multi suites have separate runners and use the same public
pattern names. Run the multi suite with `run_benchmarks_multi.sh` and the
single suite with `run_benchmarks.sh`.

Supported multi patterns:

- `DEALER_DEALER`
- `DEALER_ROUTER_SENDSEND` (`DEALER_ROUTER` input alias)
- `DEALER_ROUTER_REQREP`
- `ROUTER_ROUTER_SENDSEND` (`ROUTER_ROUTER` input alias)
- `ROUTER_ROUTER_REQREP`
- `PUBSUB`
- `STREAM`

`PERF_MULTI_*` environment variables and `perf_multi_*` artifact names retain
the multi prefix because they identify the suite rather than a pattern name.
