2026-09-05T12:59:26+09:00
2026-09-05T13:00:47+0900 idle owner persistence implemented; targeted gate running
2026-09-05T13:01:59+0900 lifecycle test added; shared Core content drift identified; official guard unchanged
2026-09-05T13:03:11+0900 idle close exposed control cleanup race; control pair lifetime moved to socket close
2026-09-05T13:05:52+0900 close owner quiescence aligned with Core EBUSY contract; official benchmark blocked by content freshness guard EXIT:1
2026-09-05T13:07:39+0900 native-idle candidate stalled at 18 virtual carriers; reuse owner resources with existing ownerLock idle condition wait
2026-09-05T13:09:48+0900 O(1) lifecycle confirmed (100/100 pollers, 100 pairs); virtual-idle 4.49k and platform-native 3.19k diagnostic DD64K remain regressions
2026-09-05T13:17:09+0900 shared Context CompletionPump implemented; native registration/wait serialized by one thread; targeted gate running
2026-09-05T13:20:10+0900 shared owner DD64K counter 25.45k, poller 1/1 and control pair 1 per Context; public handover/lifecycle test expanded
2026-09-05T13:21:21+0900 whole Java gate running; final diagnostic all-pattern measurement prepared without changing official guard
2026-09-05T13:23:21+0900 contract repetition 1/5 PASS
2026-09-05T13:23:31+0900 contract repetition 2/5 PASS
2026-09-05T13:23:41+0900 contract repetition 3/5 PASS
2026-09-05T13:23:50+0900 contract repetition 4/5 PASS
2026-09-05T13:24:01+0900 contract repetition 5/5 PASS
2026-09-05T13:24:26+0900 full gate 136 pass + 1 disabled, samples 7/7; contract 9 tests x5 PASS; API 108 classfiles unchanged
2026-09-05T13:24:58+0900 diagnostic after DEALER_DEALER 64 complete
2026-09-05T13:25:04+0900 diagnostic after DEALER_DEALER 256 complete
2026-09-05T13:25:10+0900 diagnostic after DEALER_DEALER 1024 complete
2026-09-05T13:25:16+0900 diagnostic after DEALER_DEALER 4096 complete
2026-09-05T13:25:22+0900 diagnostic after DEALER_DEALER 65536 complete
2026-09-05T13:25:31+0900 diagnostic after DEALER_ROUTER_REQREP 64 complete
2026-09-05T13:25:37+0900 diagnostic after DEALER_ROUTER_REQREP 256 complete
2026-09-05T13:25:44+0900 diagnostic after DEALER_ROUTER_REQREP 1024 complete
2026-09-05T13:25:50+0900 diagnostic after DEALER_ROUTER_REQREP 4096 complete
2026-09-05T13:25:56+0900 diagnostic after DEALER_ROUTER_REQREP 65536 complete
2026-09-05T13:26:05+0900 diagnostic after ROUTER_ROUTER_REQREP 64 complete
2026-09-05T13:26:12+0900 diagnostic after ROUTER_ROUTER_REQREP 256 complete
2026-09-05T13:26:15+0900 final diagnostic DD recovery: 4096B 305913.6 and 64KiB 20706.0 msg/s, both above pass1-before thresholds; remaining patterns running
2026-09-05T13:26:19+0900 diagnostic after ROUTER_ROUTER_REQREP 1024 complete
2026-09-05T13:26:25+0900 diagnostic after ROUTER_ROUTER_REQREP 4096 complete
2026-09-05T13:26:31+0900 diagnostic after ROUTER_ROUTER_REQREP 65536 complete
2026-09-05T13:26:40+0900 diagnostic after PUBSUB 64 complete
2026-09-05T13:26:47+0900 diagnostic after PUBSUB 256 complete
2026-09-05T13:26:54+0900 diagnostic after PUBSUB 1024 complete
2026-09-05T13:27:00+0900 diagnostic after PUBSUB 4096 complete
2026-09-05T13:27:07+0900 diagnostic after PUBSUB 65536 complete
2026-09-05T13:32:51+0900 summary and diagnostic report written; all 20 cases retained, including PUBSUB and small-DD regressions; final cleanup underway
EXIT:1
