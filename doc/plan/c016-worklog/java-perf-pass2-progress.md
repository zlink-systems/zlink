2026-09-05T15:44:01+09:00
2026-09-05T15:45:47.403713 REVIEW detached preserved; pass1 evidence and shared drain path inspected; load=168.93; official measurement deferred
2026-09-05T15:47:05.961172 REVIEW baseline build and scratch allocation candidate measurement; load1=117.65
2026-09-05T15:49:05.988188 REVIEW baseline build and scratch allocation candidate measurement; load1=139.82
2026-09-05T15:51:06.007649 REVIEW baseline build and scratch allocation candidate measurement; load1=107.59
2026-09-05T15:53:06.048309 REVIEW baseline build and scratch allocation candidate measurement; load1=140.58
2026-09-05T15:55:06.064323 REVIEW baseline build and scratch allocation candidate measurement; load1=111.09
2026-09-05T15:57:06.065460 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=133.32
2026-09-05T15:59:06.091644 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=90.78
2026-09-05T16:01:06.092765 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=44.13
2026-09-05T16:03:06.093306 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=6.95
2026-09-05T16:05:06.093849 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=1.64
2026-09-05T16:07:06.094328 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=0.69
2026-09-05T16:09:06.096303 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=4.04
2026-09-05T16:11:06.096867 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=6.46
2026-09-05T16:13:06.097366 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=1.86
2026-09-05T16:15:06.097814 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=0.52
2026-09-05T16:17:06.098204 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=3.55
2026-09-05T16:19:06.098966 MEASURE scratch views reduce DD submit 272 to 192 B/op; baseline/candidate diagnostics under high host load; no production edits; load1=4.74
2026-09-05T16:21:06.099571 IMPLEMENT only blocking Router recv reuses existing direct caller-storage path; measured recv 496->304 B/op and 64B receive latency -6.6%; other candidates no-go; load1=1.25
2026-09-05T16:23:06.100205 IMPLEMENT only blocking Router recv reuses existing direct caller-storage path; measured recv 496->304 B/op and 64B receive latency -6.6%; other candidates no-go; load1=3.73
2026-09-05T16:25:06.100588 IMPLEMENT only blocking Router recv reuses existing direct caller-storage path; measured recv 496->304 B/op and 64B receive latency -6.6%; other candidates no-go; load1=2.24
2026-09-05T16:26:19.080162 OFFICIAL_AFTER gate load1=1.28; 16:26:19 up 22:42,  1 user,  load average: 1.28, 2.79, 16.99
2026-09-05T16:26:26.998914 OFFICIAL_AFTER EXIT:1; log=bindings/java/build/pass2/official-after.log
2026-09-05T16:27:06.101174 IMPLEMENT only blocking Router recv reuses existing direct caller-storage path; measured recv 496->304 B/op and 64B receive latency -6.6%; other candidates no-go; load1=0.97
2026-09-05T16:27:36+0900 DIAGNOSTIC_AFTER DEALER_DEALER 64; load1=0.83; 16:27:36 up 22:44,  1 user,  load average: 0.83, 2.30, 15.64
2026-09-05T16:27:42+0900 diagnostic after DEALER_DEALER 64 complete
2026-09-05T16:27:42+0900 DIAGNOSTIC_AFTER DEALER_DEALER 256; load1=1.08; 16:27:42 up 22:44,  1 user,  load average: 1.08, 2.33, 15.58
2026-09-05T16:27:48+0900 diagnostic after DEALER_DEALER 256 complete
2026-09-05T16:27:48+0900 DIAGNOSTIC_AFTER DEALER_DEALER 1024; load1=1.15; 16:27:48 up 22:44,  1 user,  load average: 1.15, 2.32, 15.50
2026-09-05T16:27:54+0900 diagnostic after DEALER_DEALER 1024 complete
2026-09-05T16:27:54+0900 DIAGNOSTIC_AFTER DEALER_DEALER 4096; load1=1.78; 16:27:54 up 22:44,  1 user,  load average: 1.78, 2.43, 15.47
2026-09-05T16:28:00+0900 diagnostic after DEALER_DEALER 4096 complete
2026-09-05T16:28:00+0900 DIAGNOSTIC_AFTER DEALER_DEALER 65536; load1=2.28; 16:28:00 up 22:44,  1 user,  load average: 2.28, 2.52, 15.36
2026-09-05T16:28:06+0900 diagnostic after DEALER_DEALER 65536 complete
2026-09-05T16:28:09+0900 DIAGNOSTIC_AFTER DEALER_ROUTER_REQREP 64; load1=2.15; 16:28:09 up 22:44,  1 user,  load average: 2.15, 2.49, 15.21
2026-09-05T16:28:15+0900 diagnostic after DEALER_ROUTER_REQREP 64 complete
2026-09-05T16:28:15+0900 DIAGNOSTIC_AFTER DEALER_ROUTER_REQREP 256; load1=2.86; 16:28:15 up 22:44,  1 user,  load average: 2.86, 2.63, 15.18
2026-09-05T16:28:21+0900 diagnostic after DEALER_ROUTER_REQREP 256 complete
2026-09-05T16:28:21+0900 DIAGNOSTIC_AFTER DEALER_ROUTER_REQREP 1024; load1=2.95; 16:28:21 up 22:44,  1 user,  load average: 2.95, 2.65, 15.12
2026-09-05T16:28:27+0900 diagnostic after DEALER_ROUTER_REQREP 1024 complete
2026-09-05T16:28:37+0900 DIAGNOSTIC_AFTER DEALER_ROUTER_REQREP 4096; load1=2.70; 16:28:37 up 22:45,  1 user,  load average: 2.70, 2.62, 14.91
2026-09-05T16:28:43+0900 diagnostic after DEALER_ROUTER_REQREP 4096 complete
2026-09-05T16:28:43+0900 DIAGNOSTIC_AFTER DEALER_ROUTER_REQREP 65536; load1=2.81; 16:28:43 up 22:45,  1 user,  load average: 2.81, 2.64, 14.85
2026-09-05T16:28:50+0900 diagnostic after DEALER_ROUTER_REQREP 65536 complete
2026-09-05T16:29:06.101627 DIAGNOSTIC_AFTER DEALER_ROUTER_REQREP 65536; official shell blocked by freshness guard; load1=3.11
2026-09-05T16:29:13+0900 DIAGNOSTIC_AFTER ROUTER_ROUTER_REQREP 64; load1=2.86; 16:29:13 up 22:45,  1 user,  load average: 2.86, 2.69, 14.48
2026-09-05T16:29:19+0900 diagnostic after ROUTER_ROUTER_REQREP 64 complete
2026-09-05T16:29:29+0900 DIAGNOSTIC_AFTER ROUTER_ROUTER_REQREP 256; load1=2.71; 16:29:29 up 22:45,  1 user,  load average: 2.71, 2.67, 14.22
2026-09-05T16:29:35+0900 diagnostic after ROUTER_ROUTER_REQREP 256 complete
2026-09-05T16:29:35+0900 DIAGNOSTIC_AFTER ROUTER_ROUTER_REQREP 1024; load1=2.90; 16:29:35 up 22:46,  1 user,  load average: 2.90, 2.71, 14.17
2026-09-05T16:29:42+0900 diagnostic after ROUTER_ROUTER_REQREP 1024 complete
2026-09-05T16:29:52+0900 DIAGNOSTIC_AFTER ROUTER_ROUTER_REQREP 4096; load1=2.87; 16:29:52 up 22:46,  1 user,  load average: 2.87, 2.72, 13.99
2026-09-05T16:29:58+0900 diagnostic after ROUTER_ROUTER_REQREP 4096 complete
2026-09-05T16:30:08+0900 DIAGNOSTIC_AFTER ROUTER_ROUTER_REQREP 65536; load1=2.77; 16:30:08 up 22:46,  1 user,  load average: 2.77, 2.72, 13.81
2026-09-05T16:30:14+0900 diagnostic after ROUTER_ROUTER_REQREP 65536 complete
2026-09-05T16:30:27+0900 DIAGNOSTIC_AFTER PUBSUB 64; load1=2.86; 16:30:27 up 22:46,  1 user,  load average: 2.86, 2.76, 13.59
2026-09-05T16:30:33+0900 diagnostic after PUBSUB 64 complete
2026-09-05T16:30:43+0900 DIAGNOSTIC_AFTER PUBSUB 256; load1=2.56; 16:30:43 up 22:47,  1 user,  load average: 2.56, 2.71, 13.40
2026-09-05T16:30:49+0900 diagnostic after PUBSUB 256 complete
2026-09-05T16:30:49+0900 DIAGNOSTIC_AFTER PUBSUB 1024; load1=3.00; 16:30:49 up 22:47,  1 user,  load average: 3.00, 2.79, 13.37
2026-09-05T16:30:55+0900 diagnostic after PUBSUB 1024 complete
2026-09-05T16:31:05+0900 DIAGNOSTIC_AFTER PUBSUB 4096; load1=2.67; 16:31:05 up 22:47,  1 user,  load average: 2.67, 2.74, 13.13
2026-09-05T16:31:06.102165 DIAGNOSTIC_AFTER PUBSUB 4096; official shell blocked by freshness guard; load1=2.67
2026-09-05T16:31:11+0900 diagnostic after PUBSUB 4096 complete
2026-09-05T16:31:21+0900 DIAGNOSTIC_AFTER PUBSUB 65536; load1=2.56; 16:31:21 up 22:47,  1 user,  load average: 2.56, 2.72, 12.95
2026-09-05T16:31:27+0900 diagnostic after PUBSUB 65536 complete
2026-09-05T16:33:06.103128 DIAGNOSTIC_AFTER PUBSUB 65536; official shell blocked by freshness guard; load1=1.30
2026-09-05T16:35:06.104871 DIAGNOSTIC_AFTER PUBSUB 65536; official shell blocked by freshness guard; load1=1.33
2026-09-05T16:37:06.105894 DIAGNOSTIC_AFTER PUBSUB 65536; official shell blocked by freshness guard; load1=1.51
2026-09-05T16:39:06.106529 FINAL_REVIEW full gate 137 PASS + 1 disabled, samples 7/7, contracts 29 x5 PASS; diagnostic 20/20 complete; official after blocked; writing summary; load1=0.97
2026-09-05T16:41:06.107528 FINAL_REVIEW full gate 137 PASS + 1 disabled, samples 7/7, contracts 29 x5 PASS; diagnostic 20/20 complete; official after blocked; writing summary; load1=3.25
2026-09-05T16:43:06.108030 FINAL_REVIEW full gate 137 PASS + 1 disabled, samples 7/7, contracts 29 x5 PASS; diagnostic 20/20 complete; official after blocked; writing summary; load1=0.80
2026-09-05T16:45:06.108406 FINAL_REVIEW full gate 137 PASS + 1 disabled, samples 7/7, contracts 29 x5 PASS; diagnostic 20/20 complete; official after blocked; writing summary; load1=2.02
2026-09-05T16:47:06.108801 FINAL_REVIEW full gate 137 PASS + 1 disabled, samples 7/7, contracts 29 x5 PASS; diagnostic 20/20 complete; official after blocked; writing summary; load1=1.47
2026-09-05T16:49:06.109288 FINAL_REVIEW full gate 137 PASS + 1 disabled, samples 7/7, contracts 29 x5 PASS; diagnostic 20/20 complete; official after blocked; writing summary; load1=1.34
2026-09-05T16:49:17.485524 FINAL Java runtime 1 file + test 1 file; gate 137 PASS/disabled 1; samples 7/7; contracts 29x5 PASS; public classes 110 identical; diagnostic 20/20; official Core freshness blocked; gradle --stop PASS; summary and evidence saved
EXIT:1
