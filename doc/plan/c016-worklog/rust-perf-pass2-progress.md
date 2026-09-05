2026-09-05T22:29:27.782365 START detached preserved; existing untracked core/build, core/build-dev preserved; scope Rust only; load1=0.66; review underway
2026-09-05T22:31:44.888363 REVIEW snapshot required by socket spec:931; atomic-only close check unsafe; request entry already single Arc+Mutex; reply init+move -> adopt candidate. Build PASS using rustup Cargo.
2026-09-05T22:31:44 profile guard 22:31:44 up 1 day,  4:48,  1 user,  load average: 0.53, 1.57, 3.12 active=[]
2026-09-05T22:31:44 profile start rust-dealer_dealer 10 clients tcp 64B 1s
2026-09-05T22:31:46 profile end rust-dealer_dealer exit=0
2026-09-05T22:31:46 profile guard 22:31:46 up 1 day,  4:48,  1 user,  load average: 0.53, 1.57, 3.12 active=[]
2026-09-05T22:31:46 profile start rust-dealer_router_reqrep 10 clients tcp 64B 1s
2026-09-05T22:31:48 profile end rust-dealer_router_reqrep exit=0
2026-09-05T22:33:52 profile guard 22:33:52 up 1 day,  4:50,  1 user,  load average: 1.14, 1.39, 2.85 active=[]
2026-09-05T22:33:52 profile start c-dealer_dealer 10 clients tcp 64B 1s
2026-09-05T22:33:54 profile end c-dealer_dealer exit=0
2026-09-05T22:33:54 profile guard 22:33:54 up 1 day,  4:50,  1 user,  load average: 1.14, 1.39, 2.85 active=[]
2026-09-05T22:33:54 profile start c-dealer_router_reqrep 10 clients tcp 64B 1s
2026-09-05T22:33:55 profile end c-dealer_router_reqrep exit=0
2026-09-05T22:35:05.807990 mini start guard 22:35:05 up 1 day,  4:51,  1 user,  load average: 1.53, 1.42, 2.75
2026-09-05T22:36:47.013739 REVIEW complete: DD Rust/C FFI 15.038/7.008; DR 27.721/12.177. Native mini reply adopt rate +53.5..57.8%; shared scratch -1.25/+1.70/+4.95% (<5%, no-go). Adopt selected; RwLock/pending bundle/public wrapper pool no-go.
2026-09-05T22:39:28.900389 IMPLEMENTED reply Vec direct adopt; ownership+failure-prefix unit tests 2/2 PASS; only completion_owner.rs + private ffi.rs changed. after profile next.
2026-09-05T22:39:28 profile guard 22:39:28 up 1 day,  4:55,  1 user,  load average: 1.26, 1.24, 2.35 active=[]
2026-09-05T22:39:28 profile start rust-dealer_dealer 10 clients tcp 64B 1s
2026-09-05T22:39:31 profile end rust-dealer_dealer exit=0
2026-09-05T22:39:31 profile guard 22:39:31 up 1 day,  4:55,  1 user,  load average: 1.26, 1.24, 2.35 active=[]
2026-09-05T22:39:31 profile start rust-dealer_router_reqrep 10 clients tcp 64B 1s
2026-09-05T22:39:32 profile end rust-dealer_router_reqrep exit=0
2026-09-05T22:39:57 after guard 22:39:57 up 1 day,  4:56,  1 user,  load average: 0.90, 1.15, 2.28 active=[]
2026-09-05T22:39:57 after running load1=0.89892578125
2026-09-05T22:40:12 after running load1=2.13330078125
2026-09-05T22:40:27 after running load1=2.72021484375
2026-09-05T22:40:42 after running load1=3.03369140625
2026-09-05T22:40:42 after INVALID load1=3.03369140625 >3; terminate own measurement group
2026-09-05T22:40:42 after finished exit=-15 max_load=3.03369140625
2026-09-05T22:41:37 gate start official
2026-09-05T22:41:48 gate official exit=0
2026-09-05T22:41:48 gate start related-1
2026-09-05T22:41:50 gate related-1 exit=0
2026-09-05T22:41:50 gate start related-2
2026-09-05T22:41:52 gate related-2 exit=0
2026-09-05T22:41:52 gate start related-3
2026-09-05T22:41:54 gate related-3 exit=0
2026-09-05T22:41:54 gate start related-4
2026-09-05T22:41:56 gate related-4 exit=0
2026-09-05T22:41:56 gate start related-5
2026-09-05T22:41:58 gate related-5 exit=0
2026-09-05T22:41:58 gate start clippy
2026-09-05T22:42:00 gate clippy exit=0
2026-09-05T22:42:00 gate start fmt
2026-09-05T22:42:00 gate fmt exit=0
2026-09-05T22:42:00 gate start diff
2026-09-05T22:42:00 gate diff exit=0
2026-09-05T22:42:37 cell guard DEALER_DEALER/64 22:42:37 up 1 day,  4:59,  1 user,  load average: 1.55, 1.44, 2.23 active=[] gates_done=True
2026-09-05T22:42:52 cell guard DEALER_DEALER/64 22:42:52 up 1 day,  4:59,  1 user,  load average: 2.26, 1.60, 2.27 active=[] gates_done=True
2026-09-05T22:43:07 cell guard DEALER_DEALER/64 22:43:07 up 1 day,  4:59,  1 user,  load average: 2.45, 1.67, 2.28 active=[] gates_done=True
2026-09-05T22:43:22 cell guard DEALER_DEALER/64 22:43:22 up 1 day,  4:59,  1 user,  load average: 3.22, 1.88, 2.34 active=[] gates_done=True
2026-09-05T22:43:37 cell guard DEALER_DEALER/64 22:43:37 up 1 day,  5:00,  1 user,  load average: 2.51, 1.79, 2.30 active=[] gates_done=True
2026-09-05T22:43:52 cell guard DEALER_DEALER/64 22:43:52 up 1 day,  5:00,  1 user,  load average: 1.95, 1.70, 2.26 active=[] gates_done=True
2026-09-05T22:44:07 cell guard DEALER_DEALER/64 22:44:07 up 1 day,  5:00,  1 user,  load average: 1.52, 1.62, 2.22 active=[] gates_done=True
2026-09-05T22:44:22 cell guard DEALER_DEALER/64 22:44:22 up 1 day,  5:00,  1 user,  load average: 1.76, 1.67, 2.23 active=[] gates_done=True
2026-09-05T22:44:37 cell guard DEALER_DEALER/64 22:44:37 up 1 day,  5:01,  1 user,  load average: 1.37, 1.59, 2.20 active=[] gates_done=True
2026-09-05T22:44:52 cell guard DEALER_DEALER/64 22:44:52 up 1 day,  5:01,  1 user,  load average: 1.06, 1.51, 2.16 active=[] gates_done=True
2026-09-05T22:44:59 cell end DEALER_DEALER/64 exit=0 maxload=1.618 valid_load=True
2026-09-05T22:44:59 cell guard DEALER_DEALER/256 22:44:59 up 1 day,  5:01,  1 user,  load average: 1.62, 1.61, 2.19 active=[] gates_done=True
2026-09-05T22:45:14 cell guard DEALER_DEALER/256 22:45:14 up 1 day,  5:01,  1 user,  load average: 1.33, 1.55, 2.16 active=[] gates_done=True
2026-09-05T22:45:29 cell guard DEALER_DEALER/256 22:45:29 up 1 day,  5:01,  1 user,  load average: 1.12, 1.49, 2.13 active=[] gates_done=True
2026-09-05T22:45:34 cell end DEALER_DEALER/256 exit=0 maxload=1.668 valid_load=True
2026-09-05T22:45:34 cell guard DEALER_DEALER/1024 22:45:34 up 1 day,  5:02,  1 user,  load average: 1.67, 1.60, 2.16 active=[] gates_done=True
2026-09-05T22:45:49 cell guard DEALER_DEALER/1024 22:45:49 up 1 day,  5:02,  1 user,  load average: 1.30, 1.52, 2.13 active=[] gates_done=True
2026-09-05T22:45:55 cell end DEALER_DEALER/1024 exit=0 maxload=1.916 valid_load=True
2026-09-05T22:45:55 cell guard DEALER_DEALER/4096 22:45:55 up 1 day,  5:02,  1 user,  load average: 1.92, 1.65, 2.16 active=[] gates_done=True
2026-09-05T22:46:10 cell guard DEALER_DEALER/4096 22:46:10 up 1 day,  5:02,  1 user,  load average: 2.08, 1.70, 2.17 active=[] gates_done=True
2026-09-05T22:46:25 cell guard DEALER_DEALER/4096 22:46:25 up 1 day,  5:02,  1 user,  load average: 1.62, 1.61, 2.14 active=[] gates_done=True
2026-09-05T22:46:40 cell guard DEALER_DEALER/4096 22:46:40 up 1 day,  5:03,  1 user,  load average: 1.26, 1.53, 2.10 active=[] gates_done=True
2026-09-05T22:46:46 cell end DEALER_DEALER/4096 exit=0 maxload=1.800 valid_load=True
2026-09-05T22:46:46 cell guard DEALER_DEALER/65536 22:46:46 up 1 day,  5:03,  1 user,  load average: 1.80, 1.64, 2.13 active=[] gates_done=True
2026-09-05T22:47:01 cell guard DEALER_DEALER/65536 22:47:01 up 1 day,  5:03,  1 user,  load average: 1.62, 1.61, 2.11 active=[] gates_done=True
2026-09-05T22:47:16 cell guard DEALER_DEALER/65536 22:47:16 up 1 day,  5:03,  1 user,  load average: 1.26, 1.53, 2.08 active=[] gates_done=True
2026-09-05T22:47:21 cell end DEALER_DEALER/65536 exit=0 maxload=1.802 valid_load=True
2026-09-05T22:47:21 cell guard DEALER_ROUTER_REQREP/64 22:47:21 up 1 day,  5:03,  1 user,  load average: 1.80, 1.64, 2.11 active=[] gates_done=True
2026-09-05T22:47:36 cell guard DEALER_ROUTER_REQREP/64 22:47:36 up 1 day,  5:04,  1 user,  load average: 1.40, 1.56, 2.08 active=[] gates_done=True
2026-09-05T22:47:51 cell guard DEALER_ROUTER_REQREP/64 22:47:51 up 1 day,  5:04,  1 user,  load average: 1.09, 1.48, 2.04 active=[] gates_done=True
2026-09-05T22:47:57 cell end DEALER_ROUTER_REQREP/64 exit=0 maxload=1.324 valid_load=True
2026-09-05T22:47:57 cell guard DEALER_ROUTER_REQREP/256 22:47:57 up 1 day,  5:04,  1 user,  load average: 1.32, 1.52, 2.05 active=[] gates_done=True
2026-09-05T22:48:12 cell guard DEALER_ROUTER_REQREP/256 22:48:12 up 1 day,  5:04,  1 user,  load average: 1.03, 1.45, 2.02 active=[] gates_done=True
2026-09-05T22:48:17 cell end DEALER_ROUTER_REQREP/256 exit=0 maxload=1.030 valid_load=True
2026-09-05T22:48:17 cell guard DEALER_ROUTER_REQREP/1024 22:48:17 up 1 day,  5:04,  1 user,  load average: 0.95, 1.41, 2.00 active=[] gates_done=True
2026-09-05T22:48:23 cell end DEALER_ROUTER_REQREP/1024 exit=0 maxload=0.950 valid_load=True
2026-09-05T22:48:23 cell guard DEALER_ROUTER_REQREP/4096 22:48:23 up 1 day,  5:04,  1 user,  load average: 0.95, 1.41, 2.00 active=[] gates_done=True
2026-09-05T22:48:28 cell end DEALER_ROUTER_REQREP/4096 exit=0 maxload=1.354 valid_load=True
2026-09-05T22:48:28 cell guard DEALER_ROUTER_REQREP/65536 22:48:28 up 1 day,  5:04,  1 user,  load average: 1.35, 1.48, 2.02 active=[] gates_done=True
2026-09-05T22:48:43 cell guard DEALER_ROUTER_REQREP/65536 22:48:43 up 1 day,  5:05,  1 user,  load average: 1.05, 1.41, 1.99 active=[] gates_done=True
2026-09-05T22:48:49 cell end DEALER_ROUTER_REQREP/65536 exit=0 maxload=1.130 valid_load=True
2026-09-05T22:48:49 cell guard ROUTER_ROUTER_REQREP/64 22:48:49 up 1 day,  5:05,  1 user,  load average: 1.13, 1.42, 1.99 active=[] gates_done=True
2026-09-05T22:48:54 cell end ROUTER_ROUTER_REQREP/64 exit=0 maxload=1.440 valid_load=True
2026-09-05T22:48:55 cell guard ROUTER_ROUTER_REQREP/256 22:48:54 up 1 day,  5:05,  1 user,  load average: 1.44, 1.48, 2.00 active=[] gates_done=True
2026-09-05T22:49:10 cell guard ROUTER_ROUTER_REQREP/256 22:49:10 up 1 day,  5:05,  1 user,  load average: 1.19, 1.42, 1.98 active=[] gates_done=True
2026-09-05T22:49:15 cell end ROUTER_ROUTER_REQREP/256 exit=0 maxload=1.819 valid_load=True
2026-09-05T22:49:15 cell guard ROUTER_ROUTER_REQREP/1024 22:49:15 up 1 day,  5:05,  1 user,  load average: 1.82, 1.55, 2.01 active=[] gates_done=True
2026-09-05T22:49:30 cell guard ROUTER_ROUTER_REQREP/1024 22:49:30 up 1 day,  5:05,  1 user,  load average: 1.42, 1.47, 1.98 active=[] gates_done=True
2026-09-05T22:49:45 cell guard ROUTER_ROUTER_REQREP/1024 22:49:45 up 1 day,  5:06,  1 user,  load average: 1.17, 1.42, 1.95 active=[] gates_done=True
2026-09-05T22:49:51 cell end ROUTER_ROUTER_REQREP/1024 exit=0 maxload=1.396 valid_load=True
2026-09-05T22:49:51 cell guard ROUTER_ROUTER_REQREP/4096 22:49:51 up 1 day,  5:06,  1 user,  load average: 1.40, 1.46, 1.97 active=[] gates_done=True
2026-09-05T22:50:06 cell guard ROUTER_ROUTER_REQREP/4096 22:50:06 up 1 day,  5:06,  1 user,  load average: 1.09, 1.39, 1.93 active=[] gates_done=True
2026-09-05T22:50:11 cell end ROUTER_ROUTER_REQREP/4096 exit=0 maxload=1.560 valid_load=True
2026-09-05T22:50:11 cell guard ROUTER_ROUTER_REQREP/65536 22:50:11 up 1 day,  5:06,  1 user,  load average: 1.56, 1.48, 1.96 active=[] gates_done=True
2026-09-05T22:50:26 cell guard ROUTER_ROUTER_REQREP/65536 22:50:26 up 1 day,  5:06,  1 user,  load average: 1.21, 1.41, 1.93 active=[] gates_done=True
2026-09-05T22:50:32 cell end ROUTER_ROUTER_REQREP/65536 exit=0 maxload=1.517 valid_load=True
2026-09-05T22:50:32 cell guard PUBSUB/64 22:50:32 up 1 day,  5:06,  1 user,  load average: 1.52, 1.47, 1.94 active=[] gates_done=True
2026-09-05T22:50:47 cell guard PUBSUB/64 22:50:47 up 1 day,  5:07,  1 user,  load average: 1.25, 1.41, 1.92 active=[] gates_done=True
2026-09-05T22:50:56 cell end PUBSUB/64 exit=0 maxload=1.807 valid_load=True
2026-09-05T22:50:56 cell guard PUBSUB/256 22:50:56 up 1 day,  5:07,  1 user,  load average: 1.81, 1.53, 1.95 active=[] gates_done=True
2026-09-05T22:51:11 cell guard PUBSUB/256 22:51:11 up 1 day,  5:07,  1 user,  load average: 1.41, 1.45, 1.92 active=[] gates_done=True
2026-09-05T22:51:26 cell guard PUBSUB/256 22:51:26 up 1 day,  5:07,  1 user,  load average: 1.17, 1.40, 1.89 active=[] gates_done=True
2026-09-05T22:51:33 cell end PUBSUB/256 exit=0 maxload=1.641 valid_load=True
2026-09-05T22:51:33 cell guard PUBSUB/1024 22:51:33 up 1 day,  5:07,  1 user,  load average: 1.51, 1.47, 1.91 active=[] gates_done=True
2026-09-05T22:51:48 cell guard PUBSUB/1024 22:51:48 up 1 day,  5:08,  1 user,  load average: 1.17, 1.39, 1.88 active=[] gates_done=True
2026-09-05T22:51:54 cell end PUBSUB/1024 exit=0 maxload=1.240 valid_load=True
2026-09-05T22:51:54 cell guard PUBSUB/4096 22:51:54 up 1 day,  5:08,  1 user,  load average: 1.24, 1.40, 1.88 active=[] gates_done=True
2026-09-05T22:52:00 cell end PUBSUB/4096 exit=0 maxload=1.381 valid_load=True
2026-09-05T22:52:00 cell guard PUBSUB/65536 22:52:00 up 1 day,  5:08,  1 user,  load average: 1.38, 1.43, 1.89 active=[] gates_done=True
2026-09-05T22:52:15 cell guard PUBSUB/65536 22:52:15 up 1 day,  5:08,  1 user,  load average: 1.07, 1.36, 1.86 active=[] gates_done=True
2026-09-05T22:52:21 cell end PUBSUB/65536 exit=0 maxload=1.309 valid_load=True
2026-09-05T22:52:21 guarded after complete 20/20 valid cells
2026-09-05T22:55:17.922726 COMPLETE: 2 Rust files changed; reply direct adopt; gates 14/14 + related 5x + clippy/fmt/diff PASS; after 20/20 maxload=1.91553; ratios DD87.1 DR82.5 RR89.3 PUBSUB122.1; DD/DR held; summary and copied reports finalized
EXIT:0
