2026-09-05T19:36:33+09:00
2026-09-05T19:39:26.056492 setup: detached unchanged; cargo 1.97.1 via ~/.cargo/env; release build done -j3; perf absent, callgrind available; load 1.47; Go benchmark overlap detected, profile deferred
2026-09-05T19:40:22 profile guard 19:40:22 up 1 day,  1:56,  1 user,  load average: 2.41, 7.09, 27.87 active=[]
2026-09-05T19:40:22 profile start c-dealer_dealer 10 clients tcp 64B 1s
2026-09-05T19:40:23 profile end c-dealer_dealer exit=0
2026-09-05T19:40:23 profile guard 19:40:23 up 1 day,  1:56,  1 user,  load average: 2.30, 6.99, 27.73 active=[]
2026-09-05T19:40:24 profile start c-dealer_router_reqrep 10 clients tcp 64B 1s
2026-09-05T19:41:45 profile end c-dealer_router_reqrep exit=0
2026-09-05T19:41:45 profile guard 19:41:45 up 1 day,  1:58,  1 user,  load average: 1.47, 5.69, 25.56 active=[]
2026-09-05T19:41:45 profile start rust-dealer_dealer 10 clients tcp 64B 1s
2026-09-05T19:41:47 profile end rust-dealer_dealer exit=0
2026-09-05T19:41:47 profile guard 19:41:47 up 1 day,  1:58,  1 user,  load average: 1.47, 5.69, 25.56 active=[]
2026-09-05T19:41:47 profile start rust-dealer_router_reqrep 10 clients tcp 64B 1s
2026-09-05T19:41:49 profile end rust-dealer_router_reqrep exit=0
2026-09-05T19:43:52.346672 before Callgrind C/Rust DD+DR done; DD Rust 16380 messages, part Vec grow 16380, runner Box+Arc 32760, success errno 32770; candidate inline 2 parts + builder inlining + errno-on-error; native snapshot retained per Core socket spec Part send (consumes all outcomes)
2026-09-05T19:47:23 profile guard 19:47:23 up 1 day,  2:03,  1 user,  load average: 0.56, 2.19, 17.96 active=[]
2026-09-05T19:47:23 profile start rust-dealer_dealer 10 clients tcp 64B 1s
2026-09-05T19:47:24 profile end rust-dealer_dealer exit=0
2026-09-05T19:47:24 profile guard 19:47:24 up 1 day,  2:03,  1 user,  load average: 0.60, 2.17, 17.86 active=[]
2026-09-05T19:47:24 profile start rust-dealer_router_reqrep 10 clients tcp 64B 1s
2026-09-05T19:47:26 profile end rust-dealer_router_reqrep exit=0
20:33 RESUME after codex quota reset
2026-09-05T20:34:21.123524 resume review: retained 5 Rust files; detached unchanged; official gate started with local Core and CARGO_BUILD_JOBS=3; scripts contain no Core rebuild path
2026-09-05T20:35:33 profile guard 20:35:33 up 1 day,  2:51,  1 user,  load average: 1.32, 2.95, 4.39 active=[]
2026-09-05T20:35:33 profile start rust-dealer_dealer 10 clients tcp 64B 1s
2026-09-05T20:35:35 profile end rust-dealer_dealer exit=0
2026-09-05T20:35:35 profile guard 20:35:35 up 1 day,  2:52,  1 user,  load average: 1.32, 2.95, 4.39 active=[]
2026-09-05T20:35:35 profile start rust-dealer_router_reqrep 10 clients tcp 64B 1s
2026-09-05T20:35:37 profile end rust-dealer_router_reqrep exit=0
2026-09-05T20:36:09.573975 after benchmark guard 20:36:09 up 1 day,  2:52,  1 user,  load average: 2.03, 2.93, 4.33 active=['perf_multi', 'perf_multi']
2026-09-05T20:36:39.587286 after benchmark guard 20:36:39 up 1 day,  2:53,  1 user,  load average: 2.98, 3.08, 4.34 active=['perf_multi', 'perf_multi']
2026-09-05T20:37:09.596203 after benchmark guard 20:37:09 up 1 day,  2:53,  1 user,  load average: 2.01, 2.85, 4.22 active=[]
2026-09-05T20:37:09.628765 after benchmark exit=1
2026-09-05T20:38:02.529251 official gate 14/14 incl samples; related 4 suites x5 PASS; clippy/fmt PASS; after runner exit1 stale Core mtime guard version.rc.in, no Core rebuild; checking content provenance
2026-09-05T20:39:35.922778 runner bug fixed separately: one shared prepare_core_runtime verifies worktree source content against CMake origin before origin mtime check; four guard checks PASS
2026-09-05T20:39:35.930374 after benchmark guard 20:39:35 up 1 day,  2:56,  1 user,  load average: 2.85, 2.87, 4.03 active=[]
2026-09-05T20:40:05.939109 after benchmark guard 20:40:05 up 1 day,  2:56,  1 user,  load average: 1.79, 2.61, 3.90 active=[]
2026-09-05T20:40:35.947276 after benchmark guard 20:40:35 up 1 day,  2:57,  1 user,  load average: 1.16, 2.37, 3.78 active=[]
2026-09-05T20:41:05.951698 after benchmark running 20:41:05 up 1 day,  2:57,  1 user,  load average: 2.77, 2.64, 3.83
2026-09-05T20:41:35.955396 after benchmark running 20:41:35 up 1 day,  2:58,  1 user,  load average: 2.76, 2.66, 3.80
2026-09-05T20:42:05.958741 after benchmark running 20:42:05 up 1 day,  2:58,  1 user,  load average: 3.76, 2.89, 3.84
2026-09-05T20:42:35.963852 after benchmark running 20:42:35 up 1 day,  2:59,  1 user,  load average: 4.18, 3.09, 3.87
2026-09-05T20:42:45.115525 after benchmark exit=0
2026-09-05T20:42:58.637366 benchmark load exceeded 3 (3.76 at 20:42:05); stopped own matrix, discard this attempt; rerun same cells separately with load guard before each case
2026-09-05T20:43:55.000332 correction: prior matrix had already completed before stop request; no process was killed; report 204035 is complete but excluded due to observed load >3
2026-09-05T20:43:55.009157 cell guard DEALER_DEALER/64 20:43:55 up 1 day,  3:00,  1 user,  load average: 2.89, 2.92, 3.75 active=[]
2026-09-05T20:44:25.017423 cell guard DEALER_DEALER/64 20:44:25 up 1 day,  3:00,  1 user,  load average: 2.79, 2.87, 3.70 active=['perf_multi', 'perf_multi']
2026-09-05T20:44:55.027633 cell guard DEALER_DEALER/64 20:44:55 up 1 day,  3:01,  1 user,  load average: 2.72, 2.87, 3.68 active=[]
2026-09-05T20:45:25.037783 cell guard DEALER_DEALER/64 20:45:25 up 1 day,  3:01,  1 user,  load average: 3.19, 3.01, 3.70 active=[]
2026-09-05T20:45:55.050646 cell guard DEALER_DEALER/64 20:45:55 up 1 day,  3:02,  1 user,  load average: 3.09, 2.97, 3.66 active=['perf_multi']
2026-09-05T20:46:25.062318 cell guard DEALER_DEALER/64 20:46:25 up 1 day,  3:02,  1 user,  load average: 2.58, 2.87, 3.61 active=[]
2026-09-05T20:46:55.076466 cell guard DEALER_DEALER/64 20:46:55 up 1 day,  3:03,  1 user,  load average: 3.00, 2.93, 3.60 active=['perf_multi', 'perf_multi']
2026-09-05T20:47:25.089450 cell guard DEALER_DEALER/64 20:47:25 up 1 day,  3:03,  1 user,  load average: 2.54, 2.83, 3.55 active=['perf_multi', 'perf_multi']
2026-09-05T20:47:55.102511 cell guard DEALER_DEALER/64 20:47:55 up 1 day,  3:04,  1 user,  load average: 2.58, 2.82, 3.52 active=['perf_multi', 'perf_multi']
2026-09-05T20:48:14.000219 summary drafted with cause/profile/allocation/FFI tables and before values; valid after blocked temporarily by concurrent perf processes/load; public declaration diff PASS, all gate sessions exit0
2026-09-05T20:48:25.110934 cell guard DEALER_DEALER/64 20:48:25 up 1 day,  3:04,  1 user,  load average: 2.56, 2.82, 3.50 active=['callgrind-amd64']
2026-09-05T20:48:55.119472 cell guard DEALER_DEALER/64 20:48:55 up 1 day,  3:05,  1 user,  load average: 2.83, 2.88, 3.50 active=[]
2026-09-05T20:49:25.127460 cell guard DEALER_DEALER/64 20:49:25 up 1 day,  3:05,  1 user,  load average: 2.53, 2.82, 3.46 active=[]
2026-09-05T20:49:55.135767 cell guard DEALER_DEALER/64 20:49:55 up 1 day,  3:06,  1 user,  load average: 2.59, 2.82, 3.44 active=[]
2026-09-05T20:50:05.008604 guarded controller resumed; start guard 2.3 with hard in-run maximum 3; no valid cells existed before this adjustment
2026-09-05T20:50:05.016675 cell guard DEALER_DEALER/64 20:50:05 up 1 day,  3:06,  1 user,  load average: 2.28, 2.74, 3.41 active=[]
2026-09-05T20:50:10.519557 cell end DEALER_DEALER/64 exit=0 maxload=2.97 valid_load=True
2026-09-05T20:50:10.529457 cell guard DEALER_DEALER/256 20:50:10 up 1 day,  3:06,  1 user,  load average: 2.97, 2.88, 3.45 active=['perf_multi', 'perf_multi']
2026-09-05T20:50:25.538835 cell guard DEALER_DEALER/256 20:50:25 up 1 day,  3:06,  1 user,  load average: 3.09, 2.90, 3.45 active=[]
2026-09-05T20:50:40.546917 cell guard DEALER_DEALER/256 20:50:40 up 1 day,  3:07,  1 user,  load average: 2.73, 2.83, 3.42 active=[]
2026-09-05T20:50:55.556422 cell guard DEALER_DEALER/256 20:50:55 up 1 day,  3:07,  1 user,  load average: 2.12, 2.69, 3.36 active=[]
2026-09-05T20:51:01.059207 cell end DEALER_DEALER/256 exit=0 maxload=2.19 valid_load=True
2026-09-05T20:51:01.067835 cell guard DEALER_DEALER/1024 20:51:01 up 1 day,  3:07,  1 user,  load average: 2.19, 2.69, 3.36 active=[]
2026-09-05T20:51:06.570515 cell end DEALER_DEALER/1024 exit=0 maxload=2.74 valid_load=True
2026-09-05T20:51:06.579377 cell guard DEALER_DEALER/4096 20:51:06 up 1 day,  3:07,  1 user,  load average: 2.74, 2.80, 3.39 active=[]
2026-09-05T20:51:21.587837 cell guard DEALER_DEALER/4096 20:51:21 up 1 day,  3:07,  1 user,  load average: 2.34, 2.71, 3.35 active=[]
2026-09-05T20:51:36.595707 cell guard DEALER_DEALER/4096 20:51:36 up 1 day,  3:08,  1 user,  load average: 1.82, 2.57, 3.29 active=[]
2026-09-05T20:51:42.598452 cell end DEALER_DEALER/4096 exit=0 maxload=2.24 valid_load=True
2026-09-05T20:51:42.607051 cell guard DEALER_DEALER/65536 20:51:42 up 1 day,  3:08,  1 user,  load average: 2.06, 2.60, 3.30 active=[]
2026-09-05T20:51:48.110089 cell end DEALER_DEALER/65536 exit=0 maxload=2.53 valid_load=True
2026-09-05T20:51:48.117424 cell guard DEALER_ROUTER_REQREP/64 20:51:48 up 1 day,  3:08,  1 user,  load average: 2.53, 2.69, 3.32 active=[]
2026-09-05T20:52:03.126381 cell guard DEALER_ROUTER_REQREP/64 20:52:03 up 1 day,  3:08,  1 user,  load average: 2.04, 2.58, 3.27 active=[]
2026-09-05T20:52:08.629329 cell end DEALER_ROUTER_REQREP/64 exit=0 maxload=2.36 valid_load=True
2026-09-05T20:52:08.637648 cell guard DEALER_ROUTER_REQREP/256 20:52:08 up 1 day,  3:08,  1 user,  load average: 2.36, 2.63, 3.29 active=[]
2026-09-05T20:52:23.648349 cell guard DEALER_ROUTER_REQREP/256 20:52:23 up 1 day,  3:08,  1 user,  load average: 1.91, 2.52, 3.24 active=[]
2026-09-05T20:52:29.151847 cell end DEALER_ROUTER_REQREP/256 exit=0 maxload=2.16 valid_load=True
2026-09-05T20:52:29.161834 cell guard DEALER_ROUTER_REQREP/1024 20:52:29 up 1 day,  3:08,  1 user,  load average: 2.16, 2.56, 3.25 active=[]
2026-09-05T20:52:34.665193 cell end DEALER_ROUTER_REQREP/1024 exit=0 maxload=2.62 valid_load=True
2026-09-05T20:52:34.675846 cell guard DEALER_ROUTER_REQREP/4096 20:52:34 up 1 day,  3:09,  1 user,  load average: 2.62, 2.65, 3.28 active=[]
2026-09-05T20:52:49.683460 cell guard DEALER_ROUTER_REQREP/4096 20:52:49 up 1 day,  3:09,  1 user,  load average: 2.28, 2.57, 3.24 active=[]
2026-09-05T20:52:55.186411 cell end DEALER_ROUTER_REQREP/4096 exit=0 maxload=2.34 valid_load=True
2026-09-05T20:52:55.194218 cell guard DEALER_ROUTER_REQREP/65536 20:52:55 up 1 day,  3:09,  1 user,  load average: 2.34, 2.58, 3.24 active=[]
2026-09-05T20:53:10.201873 cell guard DEALER_ROUTER_REQREP/65536 20:53:10 up 1 day,  3:09,  1 user,  load average: 1.82, 2.45, 3.18 active=[]
2026-09-05T20:53:15.704546 cell end DEALER_ROUTER_REQREP/65536 exit=0 maxload=2.00 valid_load=True
2026-09-05T20:53:15.711658 cell guard ROUTER_ROUTER_REQREP/64 20:53:15 up 1 day,  3:09,  1 user,  load average: 2.00, 2.48, 3.19 active=[]
2026-09-05T20:53:21.214495 cell end ROUTER_ROUTER_REQREP/64 exit=0 maxload=2.32 valid_load=True
2026-09-05T20:53:21.231214 cell guard ROUTER_ROUTER_REQREP/256 20:53:21 up 1 day,  3:09,  1 user,  load average: 2.32, 2.54, 3.20 active=[]
2026-09-05T20:53:36.240463 cell guard ROUTER_ROUTER_REQREP/256 20:53:36 up 1 day,  3:10,  1 user,  load average: 2.04, 2.46, 3.17 active=[]
2026-09-05T20:53:41.743523 cell end ROUTER_ROUTER_REQREP/256 exit=0 maxload=2.76 valid_load=True
2026-09-05T20:53:41.752900 cell guard ROUTER_ROUTER_REQREP/1024 20:53:41 up 1 day,  3:10,  1 user,  load average: 2.76, 2.60, 3.21 active=[]
2026-09-05T20:53:56.760317 cell guard ROUTER_ROUTER_REQREP/1024 20:53:56 up 1 day,  3:10,  1 user,  load average: 2.66, 2.59, 3.20 active=[]
2026-09-05T20:54:11.768340 cell guard ROUTER_ROUTER_REQREP/1024 20:54:11 up 1 day,  3:10,  1 user,  load average: 2.14, 2.48, 3.15 active=[]
2026-09-05T20:54:17.271126 cell end ROUTER_ROUTER_REQREP/1024 exit=0 maxload=2.14 valid_load=True
2026-09-05T20:54:17.279428 cell guard ROUTER_ROUTER_REQREP/4096 20:54:17 up 1 day,  3:10,  1 user,  load average: 1.96, 2.43, 3.12 active=[]
2026-09-05T20:54:22.782047 cell end ROUTER_ROUTER_REQREP/4096 exit=0 maxload=2.52 valid_load=True
2026-09-05T20:54:22.791281 cell guard ROUTER_ROUTER_REQREP/65536 20:54:22 up 1 day,  3:10,  1 user,  load average: 2.52, 2.54, 3.16 active=[]
2026-09-05T20:54:37.799109 cell guard ROUTER_ROUTER_REQREP/65536 20:54:37 up 1 day,  3:11,  1 user,  load average: 1.96, 2.41, 3.11 active=[]
2026-09-05T20:54:43.302034 cell end ROUTER_ROUTER_REQREP/65536 exit=0 maxload=2.45 valid_load=True
2026-09-05T20:54:43.309839 cell guard PUBSUB/64 20:54:43 up 1 day,  3:11,  1 user,  load average: 2.45, 2.51, 3.13 active=[]
2026-09-05T20:54:58.316656 cell guard PUBSUB/64 20:54:58 up 1 day,  3:11,  1 user,  load average: 2.04, 2.41, 3.09 active=[]
2026-09-05T20:55:08.322554 cell end PUBSUB/64 exit=0 maxload=2.44 valid_load=True
2026-09-05T20:55:08.330475 cell guard PUBSUB/256 20:55:08 up 1 day,  3:11,  1 user,  load average: 2.33, 2.47, 3.10 active=[]
2026-09-05T20:55:23.338084 cell guard PUBSUB/256 20:55:23 up 1 day,  3:11,  1 user,  load average: 2.04, 2.39, 3.07 active=[]
2026-09-05T20:55:29.341489 cell end PUBSUB/256 exit=0 maxload=2.12 valid_load=True
2026-09-05T20:55:29.349029 cell guard PUBSUB/1024 20:55:29 up 1 day,  3:11,  1 user,  load average: 2.12, 2.40, 3.07 active=[]
2026-09-05T20:55:35.852922 cell end PUBSUB/1024 exit=0 maxload=2.19 valid_load=True
2026-09-05T20:55:35.860847 cell guard PUBSUB/4096 20:55:35 up 1 day,  3:12,  1 user,  load average: 2.19, 2.41, 3.07 active=[]
2026-09-05T20:55:41.864581 cell end PUBSUB/4096 exit=0 maxload=2.33 valid_load=True
2026-09-05T20:55:41.872185 cell guard PUBSUB/65536 20:55:41 up 1 day,  3:12,  1 user,  load average: 2.33, 2.44, 3.07 active=[]
2026-09-05T20:55:56.880764 cell guard PUBSUB/65536 20:55:56 up 1 day,  3:12,  1 user,  load average: 1.88, 2.34, 3.03 active=[]
2026-09-05T20:56:02.884413 cell end PUBSUB/65536 exit=0 maxload=2.13 valid_load=True
2026-09-05T20:56:02.885402 guarded after complete 20/20 valid cells
2026-09-05T20:58:47.937532 final verified: 20/20 valid reports copied, 100 result lines, maxload=2.974; C ratios DD63.0->74.3 DR65.9->83.1 RR68.2->77.3 PUB83.2->103.8%; gate PASS; no spec gap; summary finalized
EXIT:0
