2026-09-05T15:43:52+09:00 START pass3 detached; core build symlinks preserved;  15:43:52 up 22:00,  1 user,  load average: 17.92, 4.63, 3.01
2026-09-05T15:45:47+09:00 baseline profiling running;  15:45:47 up 22:02,  1 user,  load average: 170.37, 64.80, 25.03
2026-09-05T15:48:01+09:00 entry/TCS integration and private receive storage reuse candidate; official measurement deferred;  15:48:01 up 22:04,  1 user,  load average: 114.08, 80.47, 36.15
2026-09-05T15:50:26+09:00 persistent socket completion worker candidate: idle wait and existing owner fast check, no per-backpressure Task.Run;  15:50:26 up 22:06,  1 user,  load average: 139.67, 106.36, 52.35
2026-09-05T15:53:25+09:00 receive assembly now moves native part directly into final Message wrapper; pooled private collection storage owns buffer;  15:53:26 up 22:09,  1 user,  load average: 145.93, 117.08, 65.42
2026-09-05T15:55:52+09:00 baseline alloc confirmed request288/reply32/drain40/total640 B/op; interop and gate instrumentation isolated under dotnet artifacts;  15:55:52 up 22:12,  1 user,  load average: 116.38, 116.18, 72.67
2026-09-05T15:57:47+09:00 targeted 38/38 passed; removed redundant drain lock; new receive-storage tests pending;  15:57:47 up 22:14,  1 user,  load average: 141.58, 124.52, 80.69
2026-09-05T15:59:20+09:00 native instrumentation baseline built; receive and Task identity regression coverage added; official load gate still blocked;  15:59:20 up 22:15,  1 user,  load average: 87.84, 111.67, 80.44
2026-09-05T16:04:32+09:00 immediate native sends/replies now bypass managed gate; early WRITABLE joins in same registry; admission contract gate running;  16:04:32 up 22:20,  1 user,  load average: 2.68, 46.71, 61.75
2026-09-05T16:07:45+09:00 final contract suite and low-load readiness;  16:07:45 up 22:24,  1 user,  load average: 1.71, 24.84, 50.23
2026-09-05T16:08:27+09:00 full gate EXIT:1;  16:08:27 up 22:24,  1 user,  load average: 1.49, 21.91, 48.17
2026-09-05T16:08:43+09:00 full gate 220/222: dedicated-thread source guard and concurrent send EINVAL; isolating Core/binding ownership before proceeding;  16:08:43 up 22:25,  1 user,  load average: 2.00, 20.68, 47.21
2026-09-05T16:11:58+09:00 no-lock multipart rejected (native socket-local sequence EINVAL); retained serialization; persistent async pump with reusable idle ValueTask source;  16:11:58 up 22:28,  1 user,  load average: 3.54, 13.46, 39.51
2026-09-05T16:13:35+09:00 async idle wake contract 63/63 passed; final full gate/repeats running;  16:13:35 up 22:30,  1 user,  load average: 1.21, 9.82, 35.54
2026-09-05T16:13:51+09:00 final full gate EXIT:0;  16:13:51 up 22:30,  1 user,  load average: 1.44, 9.45, 35.01
2026-09-05T16:15:07+09:00 API surface diff EXIT:0; final tests222/222 samples7/7;  16:15:07 up 22:31,  1 user,  load average: 0.52, 7.38, 32.30
2026-09-05T16:15:51+09:00 OFFICIAL after preflight load1=0.45; other_benchmarks=[]
2026-09-05T16:15:51+09:00 OFFICIAL after START pid=3267781; 16:15:51 up 22:32,  1 user,  load average: 0.45, 6.39, 30.78
2026-09-05T16:16:21+09:00 OFFICIAL after monitor 16:16:21 up 22:32,  1 user,  load average: 2.19, 6.23, 29.95; benchmarks=[]
2026-09-05T16:16:51+09:00 OFFICIAL after monitor 16:16:51 up 22:33,  1 user,  load average: 3.30, 6.10, 29.15; benchmarks=[]
2026-09-05T16:17:21+09:00 OFFICIAL after monitor 16:17:21 up 22:33,  1 user,  load average: 3.41, 5.86, 28.34; benchmarks=[]
2026-09-05T16:17:51+09:00 OFFICIAL after monitor 16:17:51 up 22:34,  1 user,  load average: 4.26, 5.86, 27.62; benchmarks=[]
2026-09-05T16:18:21+09:00 OFFICIAL after monitor 16:18:21 up 22:34,  1 user,  load average: 5.37, 6.02, 26.98; benchmarks=[]
2026-09-05T16:18:51+09:00 OFFICIAL after monitor 16:18:51 up 22:35,  1 user,  load average: 5.22, 5.92, 26.28; benchmarks=['3274876 java /home/hep7hep7/.jdks/jdk-22.0.2+9/bin/java --enable-native-access=ALL-UNNAMED -server -XX:TieredStopAtLevel=4 -classpath /home/hep7hep7/project/zlink-wt-java-pe', '3274914 java /home/hep7hep7/.jdks/jdk-22.0.2+9/bin/java --enable-native-access=ALL-UNNAMED -server -XX:TieredStopAtLevel=4 -classpath /home/hep7hep7/project/zlink-wt-java-pe']
2026-09-05T16:18:51+09:00 OFFICIAL after END EXIT:0; 16:18:51 up 22:35,  1 user,  load average: 5.22, 5.92, 26.28
2026-09-05T16:19:29+09:00 after20/20 completed but REQREP64KiB regressed to DR478/RR525.6 ops/s; preserving first report and isolating persistent pump behavior;  16:19:29 up 22:35,  1 user,  load average: 3.12, 5.32, 25.22
2026-09-05T16:27:19+09:00 Context-owned single native poller/worker implemented; socket epoch/runtime pollers removed; lifecycle and contract verification running;  16:27:19 up 22:43,  1 user,  load average: 0.75, 2.37, 15.88
2026-09-05T16:30:05+09:00 context final contract EXIT:0;  16:30:05 up 22:46,  1 user,  load average: 2.77, 2.72, 13.81
2026-09-05T16:31:25+09:00 DIAGNOSTIC context pump REQREP64KiB start (not official after); no other benchmark process;  16:31:25 up 22:47,  1 user,  load average: 2.59, 2.72, 12.90
2026-09-05T16:32:38+09:00 DIAGNOSTIC context pump REQREP64KiB EXIT:1;  16:32:38 up 22:49,  1 user,  load average: 1.97, 2.57, 12.11
2026-09-05T16:35:58+09:00 Context64KiB diagnostic failed drain deadline; tracing per-owner native completion progress (no budget changes);  16:35:58 up 22:52,  1 user,  load average: 0.68, 1.70, 9.91
2026-09-05T16:36:56+09:00 native poller source scans registration order to event_capacity; checking fixed capacity1 starvation with per-owner trace;  16:36:56 up 22:53,  1 user,  load average: 1.70, 1.89, 9.46
2026-09-05T16:38:22+09:00 Context native event batch expanded to registration count; fixed capacity1 guard conflicts, exact proposed test diff saved;  16:38:22 up 22:54,  1 user,  load average: 0.71, 1.56, 8.68
2026-09-05T16:46:01+09:00 CORE BLOCKER confirmed by public C repro: no reply/competing sender, 1000 rejected requests ->1000 immediate WRITABLE in3.356ms; no Core edits; temporary traces removed;  16:46:01 up 23:02,  1 user,  load average: 1.41, 1.49, 5.88
2026-09-05T16:50:41+09:00 binding root cause: inline WRITABLE retry before NO_DATA violates core socket spec1068; moved retry dispatch to drain boundary, no budget/fairness changes;  16:50:41 up 23:07,  1 user,  load average: 0.82, 1.39, 4.71
2026-09-05T16:52:25+09:00 DIAGNOSTIC post-NO_DATA retry 64KiB start;  16:52:25 up 23:08,  1 user,  load average: 1.59, 1.61, 4.46
2026-09-05T16:52:54+09:00 DIAGNOSTIC post-NO_DATA retry EXIT:1;  16:52:54 up 23:09,  1 user,  load average: 2.48, 1.81, 4.43
2026-09-05T16:53:13+09:00 post-NO_DATA functional contract63 passed; retained existing RetryRequest helper signature for unchanged source guard;  16:53:13 up 23:09,  1 user,  load average: 1.77, 1.69, 4.34
2026-09-05T16:54:46+09:00 DIAGNOSTIC Context full readiness batch + post-NO_DATA retry; native poller first-slot scanning identified, source guard delta pending;  16:54:46 up 23:11,  1 user,  load average: 1.55, 1.65, 4.06
2026-09-05T16:57:56+09:00 DIAGNOSTIC original binding assembly/current Core64KiB baseline to separate artifact effects;  16:57:56 up 23:14,  1 user,  load average: 0.30, 1.23, 3.47
2026-09-05T16:58:20+09:00 DIAGNOSTIC original binding/current Core EXIT:0;  16:58:20 up 23:14,  1 user,  load average: 2.54, 1.69, 3.57
2026-09-05T17:03:53+09:00 retained implementation full gate EXIT:0;  17:03:53 up 23:20,  1 user,  load average: 0.93, 1.08, 2.74
2026-09-05T17:06:44+09:00 retained final repeated-contract/probe/API completion EXIT:0;  17:06:44 up 23:23,  1 user,  load average: 1.97, 1.39, 2.57
2026-09-05T17:09:26.505417+09:00 FINAL after WAIT other_benchmark=True; 17:09:26 up 23:25,  1 user,  load average: 2.06, 1.61, 2.46
2026-09-05T17:09:36.513641+09:00 FINAL after START no_other_benchmark; start_load=1.74658203125; 17:09:36 up 23:26,  1 user,  load average: 1.75, 1.56, 2.43
2026-09-05T17:09:36.515703+09:00 FINAL after RUNNING pid=3331028; 17:09:36 up 23:26,  1 user,  load average: 1.75, 1.56, 2.43
2026-09-05T17:09:56.517675+09:00 FINAL after RUNNING pid=3331028; 17:09:56 up 23:26,  1 user,  load average: 2.02, 1.64, 2.44
2026-09-05T17:10:16.520543+09:00 FINAL after RUNNING pid=3331028; 17:10:16 up 23:26,  1 user,  load average: 2.84, 1.85, 2.49
2026-09-05T17:10:36.524122+09:00 FINAL after RUNNING pid=3331028; 17:10:36 up 23:27,  1 user,  load average: 3.67, 2.12, 2.57
2026-09-05T17:10:56.527428+09:00 FINAL after RUNNING pid=3331028; 17:10:56 up 23:27,  1 user,  load average: 3.09, 2.06, 2.54
2026-09-05T17:11:16.530762+09:00 FINAL after RUNNING pid=3331028; 17:11:16 up 23:27,  1 user,  load average: 3.05, 2.12, 2.55
2026-09-05T17:11:36.534409+09:00 FINAL after RUNNING pid=3331028; 17:11:36 up 23:28,  1 user,  load average: 3.45, 2.29, 2.60
2026-09-05T17:11:56.538348+09:00 FINAL after RUNNING pid=3331028; 17:11:56 up 23:28,  1 user,  load average: 3.21, 2.32, 2.60
2026-09-05T17:12:16.541866+09:00 FINAL after RUNNING pid=3331028; 17:12:16 up 23:28,  1 user,  load average: 3.25, 2.38, 2.62
2026-09-05T17:12:36.544957+09:00 FINAL after process EXIT:0; 17:12:36 up 23:29,  1 user,  load average: 2.64, 2.31, 2.59
2026-09-05T17:12:55.108671+09:00 final reports copied; gates PASS; scope4files; partial completion: pump/no-lock and official load gate unresolved; EXIT:1
2026-09-05T17:13:20+09:00 final artifact audit: shared Core runtime changed externally (mtime16:51:35 SHA256e680b264...), historical/after native identity not established; summary corrected; EXIT:1
