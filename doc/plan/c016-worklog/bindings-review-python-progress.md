START:main confirmed; preserved pre-existing out-of-scope changes; reviewing contract B port 5240947587
REVIEW:found async runtime pump busy-loop (zero-time poll + call_soon reschedule) and double payload copy on successful async sends
PERF_BEFORE:DEALER_ROUTER tcp 1024B duration=3 runs=1 throughput=7855 msg/s latency_mean=1.104ms p95=5.230ms p99=7.183ms
REVIEW:contract a-h paths conform; found completion unregister O(n) scan among live IDs
FIX:retained native zlink_msg snapshot with refcount clones; blocking wake-FD completion wait; O(1) registry removal
TEST:full Python suite 155 passed + 4 subtests; samples 7/7 passed
TEST:public exact-WRITABLE retry and runtime-owner preattach send passed 5/5 runs (2 tests each, no sleep)
PERF_AFTER:DEALER_ROUTER tcp 1024B duration=3 runs=1 throughput=20021 msg/s latency_mean=0.218ms p95=0.389ms p99=3.963ms
SMOKE_SINGLE:6/6 complete; PAIR tcp/inproc=17028/19584, DEALER_ROUTER=21746/11008.5, PUBSUB=50893/32299 msg/s
SMOKE_MULTI:24/24 complete, 120/120 result rows; 8 clients, 2s, 1024/65536B, tcp/tls/ws/wss; no zero throughput
REVIEW:multi PUBSUB runner omitted STOP (30s delay/case); timeout output with RESULT lines was misclassified as success
FIX:multi runner sends PUBSUB STOP before wait and preserves SystemExit case failure even with partial RESULT lines
TEST:perf runner unit 48 passed + 4 subtests; fixed PUBSUB tcp 1024/65536 smoke 2/2 in 4s (was ~60s)
SMOKE_MULTI_FINAL:24/24 complete, 120/120 result rows in 93s with fixed runner
TEST_FINAL:157 passed + 4 subtests; samples 7/7; core regressions 3 tests x 5 runs green
VERIFY:main; full git diff --check passed; no Python mirror cmp target; no blockers
EXIT:0
