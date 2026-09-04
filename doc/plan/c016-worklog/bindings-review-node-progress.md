START branch=main scope=bindings/node/** existing-user-changes-outside-scope-preserved
REVIEW contract-read; completion/send/poller/native paths inspected; suspected idle zero-timeout pump spin and ESHUTDOWN mapping gap
BASELINE DEALER_ROUTER tcp 1024B duration=3 runs=1 throughput=28373msg/s latency_mean=219.261ms p95=350.259ms p99=376.949ms
RESUME branch=main existing-node-diff-preserved; reviewing contract and pre-port hot path
FINDING hot-path CompletionEntry/Promise + 3 Map operations per successful send removed by inherited patch; retry snapshot now rejection-only
PERF pre-port=152844.667msg/s,52.271ms; fixed=145420.667msg/s,55.264ms; fixed exceeds required 112715msg/s floor
REGRESSION public completion/backpressure/routed suite 30 tests x5 green; no sleeps
SMOKE native addon reuse-build green; TypeScript build green; full Node tests green; samples 7/7 green
SMOKE single PAIR/DEALER_ROUTER/PUBSUB tcp green (192561/151774.5/196371.5msg/s); inproc reported runner-supported UNSUPPORTED, status complete
SMOKE multi clients=8 duration=2 sizes=1024,65536 patterns=DEALER_DEALER/DEALER_ROUTER_SENDSEND/PUBSUB: 24/24 cases green, zero failures
FINAL git-diff-check green; summary=bindings-review-node-summary.md; blockers=none
EXIT:0
