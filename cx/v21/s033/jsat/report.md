| 조건 | 프로세스 | TID | thread name (`/proc` comm) | 역할 | CPU cores | CPU µs/request | voluntary switch/request | involuntary switch/request | run-queue wait µs/request |
|---|---|---:|---|---|---:|---:|---:|---:|---:|
| Framework 무제한 | source | 114923 | `ZLINKbg/IO/2` | binding/Core I/O | 0.750 | 13.02 | 0.332 | 0.007 | 0.298 |
| Framework 무제한 | source | 114921 | `ZLINKbg/IO/0` | binding/Core I/O | 0.720 | 12.51 | 0.179 | 0.004 | 0.092 |
| Framework 무제한 | source | 115695 | `bench-phase-act` | 호출/제출 | 0.714 | 12.41 | 0.290 | 0.005 | 0.111 |
| Framework 무제한 | source | 114929 | `zlink-jvm-raw-m` | Framework mesh thread | 0.686 | 11.92 | 0.386 | 0.005 | 0.316 |
| Framework 무제한 | source | 114944 | `ForkJoinPool.co` | completion/pump | 0.149 | 2.58 | 0.144 | 0.003 | 0.550 |
| Framework 무제한 | source | 114974 | `ForkJoinPool.co` | completion/pump | 0.148 | 2.58 | 0.143 | 0.003 | 0.545 |
| Framework 무제한 | source | 114976 | `ForkJoinPool.co` | completion/pump | 0.148 | 2.57 | 0.145 | 0.003 | 0.553 |
| Framework 무제한 | source | 114972 | `ForkJoinPool.co` | completion/pump | 0.148 | 2.57 | 0.145 | 0.003 | 0.543 |
| Framework 무제한 | source | 114969 | `ForkJoinPool.co` | completion/pump | 0.148 | 2.57 | 0.144 | 0.003 | 0.545 |
| Framework 무제한 | source | 114973 | `ForkJoinPool.co` | completion/pump | 0.147 | 2.56 | 0.143 | 0.003 | 0.545 |
| Framework 무제한 | source | 114975 | `ForkJoinPool.co` | completion/pump | 0.147 | 2.55 | 0.144 | 0.003 | 0.557 |
| Framework 무제한 | source | 114760 | `C2 CompilerThre` | JIT | 0.020 | 0.35 | 0.000 | 0.000 | 0.001 |
| Framework 무제한 | source | 114763 | `Common-Cleaner` | JVM/기타 | 0.010 | 0.18 | 0.000 | 0.000 | 0.005 |
| Framework 무제한 | source | 114754 | `VM Thread` | JVM/기타 | 0.010 | 0.17 | 0.003 | 0.000 | 0.016 |
| Framework 무제한 | source | 114745 | `GC Thread#0` | GC | 0.007 | 0.12 | 0.000 | 0.000 | 0.014 |
| Framework 무제한 | source | 114844 | `GC Thread#4` | GC | 0.006 | 0.11 | 0.000 | 0.000 | 0.013 |
| Framework 무제한 | source | 114841 | `GC Thread#1` | GC | 0.006 | 0.11 | 0.000 | 0.000 | 0.019 |
| Framework 무제한 | source | 114846 | `GC Thread#6` | GC | 0.006 | 0.11 | 0.000 | 0.000 | 0.018 |
| Framework 무제한 | source | 114842 | `GC Thread#2` | GC | 0.006 | 0.11 | 0.000 | 0.000 | 0.022 |
| Framework 무제한 | source | 114843 | `GC Thread#3` | GC | 0.006 | 0.11 | 0.000 | 0.000 | 0.025 |
| Framework 무제한 | source | 114847 | `GC Thread#7` | GC | 0.006 | 0.10 | 0.000 | 0.000 | 0.029 |
| Framework 무제한 | source | 114810 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.005 | 0.10 | 0.000 | 0.000 | 0.002 |
| Framework 무제한 | source | 114845 | `GC Thread#5` | GC | 0.005 | 0.09 | 0.000 | 0.000 | 0.038 |
| Framework 무제한 | source | 114933 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.005 | 0.09 | 0.000 | 0.000 | 0.003 |
| Framework 무제한 | source | 114755 | `Reference Handl` | JVM/기타 | 0.005 | 0.08 | 0.000 | 0.000 | 0.002 |
| Framework 무제한 | source | 114811 | `JFR Sampler Thr` | JVM/기타 | 0.004 | 0.07 | 0.005 | 0.000 | 0.039 |
| Framework 무제한 | source | 114808 | `JFR Periodic Ta` | JVM/기타 | 0.002 | 0.03 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114761 | `C1 CompilerThre` | JIT | 0.002 | 0.03 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114925 | `zlink-java-chan` | JVM/기타 | 0.002 | 0.03 | 0.000 | 0.000 | 0.004 |
| Framework 무제한 | source | 114797 | `JFR Recorder Th` | JVM/기타 | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114748 | `G1 Refine#0` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.006 |
| Framework 무제한 | source | 114935 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114924 | `ZLINKbg/IO/3` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114919 | `ZLINKbg/Reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.001 |
| Framework 무제한 | source | 114922 | `ZLINKbg/IO/1` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114749 | `G1 Service` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114804 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114801 | `HttpClient-1-Se` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114805 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114759 | `Monitor Deflati` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.001 |
| Framework 무제한 | source | 114753 | `VM Periodic Tas` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114802 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114758 | `Service Thread` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114965 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114966 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114803 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114926 | `zlink-timer` | timer | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114793 | `Attach Listener` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114756 | `Finalizer` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114898 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114934 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114747 | `G1 Conc#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114899 | `G1 Conc#1` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114746 | `G1 Main Marker` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114967 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114812 | `JFR Recording S` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114762 | `Notification Th` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114757 | `Signal Dispatch` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114809 | `VirtualThread-u` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114920 | `ZLINKbg/core-ct` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114731 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114744 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 115649 | `process reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | source | 114938 | `zlink-req-time` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114693 | `ZLINKbg/IO/1` | binding/Core I/O | 0.773 | 13.42 | 0.235 | 0.002 | 0.073 |
| Framework 무제한 | target | 114708 | `zlink-jvm-raw-m` | Framework mesh thread | 0.605 | 10.50 | 0.309 | 0.003 | 0.447 |
| Framework 무제한 | target | 114692 | `ZLINKbg/IO/0` | binding/Core I/O | 0.560 | 9.73 | 0.334 | 0.002 | 0.499 |
| Framework 무제한 | target | 114719 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.333 | 5.79 | 0.058 | 0.001 | 0.030 |
| Framework 무제한 | target | 114718 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.308 | 5.34 | 0.051 | 0.001 | 0.015 |
| Framework 무제한 | target | 114669 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.256 | 4.45 | 0.046 | 0.001 | 0.016 |
| Framework 무제한 | target | 114606 | `C2 CompilerThre` | JIT | 0.021 | 0.36 | 0.000 | 0.000 | 0.003 |
| Framework 무제한 | target | 114658 | `GC Thread#5` | GC | 0.008 | 0.13 | 0.000 | 0.000 | 0.010 |
| Framework 무제한 | target | 114654 | `GC Thread#1` | GC | 0.007 | 0.13 | 0.000 | 0.000 | 0.008 |
| Framework 무제한 | target | 114660 | `GC Thread#7` | GC | 0.007 | 0.13 | 0.000 | 0.000 | 0.010 |
| Framework 무제한 | target | 114656 | `GC Thread#3` | GC | 0.007 | 0.12 | 0.000 | 0.000 | 0.014 |
| Framework 무제한 | target | 114657 | `GC Thread#4` | GC | 0.007 | 0.12 | 0.000 | 0.000 | 0.005 |
| Framework 무제한 | target | 114591 | `GC Thread#0` | GC | 0.007 | 0.12 | 0.000 | 0.000 | 0.023 |
| Framework 무제한 | target | 114659 | `GC Thread#6` | GC | 0.007 | 0.12 | 0.000 | 0.000 | 0.019 |
| Framework 무제한 | target | 114865 | `JFR Sampler Thr` | JVM/기타 | 0.005 | 0.09 | 0.006 | 0.000 | 0.023 |
| Framework 무제한 | target | 114655 | `GC Thread#2` | GC | 0.005 | 0.08 | 0.000 | 0.000 | 0.029 |
| Framework 무제한 | target | 114607 | `C1 CompilerThre` | JIT | 0.003 | 0.05 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114730 | `bench-stats` | JVM/기타 | 0.003 | 0.05 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114862 | `JFR Periodic Ta` | JVM/기타 | 0.002 | 0.03 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114696 | `zlink-java-chan` | JVM/기타 | 0.001 | 0.02 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114600 | `VM Thread` | JVM/기타 | 0.001 | 0.02 | 0.000 | 0.000 | 0.007 |
| Framework 무제한 | target | 114706 | `zlink-timer` | timer | 0.001 | 0.02 | 0.002 | 0.000 | 0.000 |
| Framework 무제한 | target | 114857 | `JFR Recorder Th` | JVM/기타 | 0.000 | 0.01 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114609 | `Common-Cleaner` | JVM/기타 | 0.000 | 0.01 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114594 | `G1 Refine#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114694 | `ZLINKbg/IO/2` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.002 |
| Framework 무제한 | target | 114690 | `ZLINKbg/Reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114601 | `Reference Handl` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114695 | `ZLINKbg/IO/3` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114726 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114595 | `G1 Service` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114668 | `VirtualThread-u` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114605 | `Monitor Deflati` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114604 | `Service Thread` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114599 | `VM Periodic Tas` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114725 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114848 | `Attach Listener` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114602 | `Finalizer` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114670 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114717 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114593 | `G1 Conc#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114680 | `G1 Conc#1` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114592 | `G1 Main Marker` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114866 | `JFR Recording S` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114608 | `Notification Th` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114603 | `Signal Dispatch` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114691 | `ZLINKbg/core-ct` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114729 | `bench-stats` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114577 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Framework 무제한 | target | 114590 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131950 | `ZLINKbg/IO/0` | binding/Core I/O | 0.483 | 8.39 | 0.212 | 0.001 | 0.012 |
| Binding 57,550 req/s pacing | source | 132711 | `bench-phase-act` | 호출/제출 | 0.472 | 8.19 | 0.171 | 0.001 | 0.018 |
| Binding 57,550 req/s pacing | source | 131952 | `ZLINKbg/IO/2` | binding/Core I/O | 0.370 | 6.43 | 0.350 | 0.000 | 0.009 |
| Binding 57,550 req/s pacing | source | 131884 | `C2 CompilerThre` | JIT | 0.013 | 0.23 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131962 | `GC Thread#7` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131958 | `GC Thread#3` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131957 | `GC Thread#2` | GC | 0.000 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 57,550 req/s pacing | source | 131961 | `GC Thread#6` | GC | 0.000 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 57,550 req/s pacing | source | 131956 | `GC Thread#1` | GC | 0.000 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 57,550 req/s pacing | source | 131960 | `GC Thread#5` | GC | 0.000 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 57,550 req/s pacing | source | 131878 | `VM Thread` | JVM/기타 | 0.000 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131869 | `GC Thread#0` | GC | 0.000 | 0.01 | 0.000 | 0.000 | 0.002 |
| Binding 57,550 req/s pacing | source | 131959 | `GC Thread#4` | GC | 0.000 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 57,550 req/s pacing | source | 131885 | `C1 CompilerThre` | JIT | 0.000 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131938 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131951 | `ZLINKbg/IO/1` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131948 | `ZLINKbg/Reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131953 | `ZLINKbg/IO/3` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131926 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131925 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131920 | `HttpClient-1-Se` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131883 | `Monitor Deflati` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131873 | `G1 Service` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131882 | `Service Thread` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131877 | `VM Periodic Tas` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131974 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131921 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131977 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131975 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131879 | `Reference Handl` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131924 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131887 | `Common-Cleaner` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131880 | `Finalizer` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131871 | `G1 Conc#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131870 | `G1 Main Marker` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131872 | `G1 Refine#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131976 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131886 | `Notification Th` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131881 | `Signal Dispatch` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131937 | `VirtualThread-u` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131949 | `ZLINKbg/core-ct` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131855 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131868 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 132688 | `process reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | source | 131963 | `zlink-req-time` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131825 | `ZLINKbg/IO/1` | binding/Core I/O | 0.441 | 7.67 | 0.290 | 0.000 | 0.008 |
| Binding 57,550 req/s pacing | target | 131824 | `ZLINKbg/IO/0` | binding/Core I/O | 0.410 | 7.13 | 0.326 | 0.000 | 0.031 |
| Binding 57,550 req/s pacing | target | 131841 | `bench-raw-reque` | binding receive/request | 0.362 | 6.30 | 0.364 | 0.000 | 0.007 |
| Binding 57,550 req/s pacing | target | 131776 | `C2 CompilerThre` | JIT | 0.009 | 0.15 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131982 | `GC Thread#4` | GC | 0.004 | 0.06 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131981 | `GC Thread#3` | GC | 0.003 | 0.06 | 0.000 | 0.000 | 0.003 |
| Binding 57,550 req/s pacing | target | 131984 | `GC Thread#6` | GC | 0.003 | 0.06 | 0.000 | 0.000 | 0.002 |
| Binding 57,550 req/s pacing | target | 131980 | `GC Thread#2` | GC | 0.003 | 0.06 | 0.000 | 0.000 | 0.006 |
| Binding 57,550 req/s pacing | target | 131979 | `GC Thread#1` | GC | 0.003 | 0.05 | 0.000 | 0.000 | 0.012 |
| Binding 57,550 req/s pacing | target | 131983 | `GC Thread#5` | GC | 0.003 | 0.05 | 0.000 | 0.000 | 0.013 |
| Binding 57,550 req/s pacing | target | 131985 | `GC Thread#7` | GC | 0.003 | 0.05 | 0.000 | 0.000 | 0.014 |
| Binding 57,550 req/s pacing | target | 131854 | `bench-stats` | JVM/기타 | 0.002 | 0.04 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131761 | `GC Thread#0` | GC | 0.002 | 0.04 | 0.000 | 0.000 | 0.027 |
| Binding 57,550 req/s pacing | target | 131777 | `C1 CompilerThre` | JIT | 0.001 | 0.02 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131770 | `VM Thread` | JVM/기타 | 0.000 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131822 | `ZLINKbg/Reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131827 | `ZLINKbg/IO/3` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131764 | `G1 Refine#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131826 | `ZLINKbg/IO/2` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131840 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131842 | `bench-raw-comma` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131775 | `Monitor Deflati` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.010 |
| Binding 57,550 req/s pacing | target | 131765 | `G1 Service` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131774 | `Service Thread` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131769 | `VM Periodic Tas` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131839 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131779 | `Common-Cleaner` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131772 | `Finalizer` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131763 | `G1 Conc#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131762 | `G1 Main Marker` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131778 | `Notification Th` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131771 | `Reference Handl` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131773 | `Signal Dispatch` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131823 | `ZLINKbg/core-ct` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131852 | `bench-stats` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131747 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 57,550 req/s pacing | target | 131759 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 142080 | `bench-phase-act` | 호출/제출 | 0.959 | 7.45 | 0.084 | 0.000 | 0.002 |
| Binding 무제한 | source | 141344 | `ZLINKbg/IO/0` | binding/Core I/O | 0.956 | 7.42 | 0.062 | 0.000 | 0.005 |
| Binding 무제한 | source | 141346 | `ZLINKbg/IO/2` | binding/Core I/O | 0.753 | 5.85 | 0.222 | 0.000 | 0.013 |
| Binding 무제한 | source | 141297 | `C2 CompilerThre` | JIT | 0.013 | 0.10 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141282 | `GC Thread#0` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 무제한 | source | 141361 | `GC Thread#4` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141363 | `GC Thread#6` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 무제한 | source | 141358 | `GC Thread#1` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 무제한 | source | 141362 | `GC Thread#5` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 무제한 | source | 141360 | `GC Thread#3` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 무제한 | source | 141359 | `GC Thread#2` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141291 | `VM Thread` | JVM/기타 | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141364 | `GC Thread#7` | GC | 0.001 | 0.01 | 0.000 | 0.000 | 0.001 |
| Binding 무제한 | source | 141298 | `C1 CompilerThre` | JIT | 0.001 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141338 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141342 | `ZLINKbg/Reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141345 | `ZLINKbg/IO/1` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141347 | `ZLINKbg/IO/3` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141324 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141320 | `HttpClient-1-Se` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141325 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141286 | `G1 Service` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141296 | `Monitor Deflati` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141381 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141295 | `Service Thread` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141382 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141290 | `VM Periodic Tas` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141384 | `ForkJoinPool-1-` | Framework dispatcher/executor | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141321 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141292 | `Reference Handl` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141323 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141300 | `Common-Cleaner` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141293 | `Finalizer` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141284 | `G1 Conc#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141283 | `G1 Main Marker` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141285 | `G1 Refine#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141383 | `HttpClient-1-Wo` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141299 | `Notification Th` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141294 | `Signal Dispatch` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141337 | `VirtualThread-u` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141343 | `ZLINKbg/core-ct` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141268 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141281 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 142038 | `process reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | source | 141367 | `zlink-req-time` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141253 | `ZLINKbg/IO/1` | binding/Core I/O | 0.880 | 6.83 | 0.110 | 0.000 | 0.003 |
| Binding 무제한 | target | 141252 | `ZLINKbg/IO/0` | binding/Core I/O | 0.863 | 6.70 | 0.185 | 0.000 | 0.012 |
| Binding 무제한 | target | 141261 | `bench-raw-reque` | binding receive/request | 0.755 | 5.86 | 0.214 | 0.000 | 0.006 |
| Binding 무제한 | target | 141392 | `GC Thread#6` | GC | 0.009 | 0.07 | 0.000 | 0.000 | 0.005 |
| Binding 무제한 | target | 141391 | `GC Thread#5` | GC | 0.009 | 0.07 | 0.000 | 0.000 | 0.005 |
| Binding 무제한 | target | 141232 | `C2 CompilerThre` | JIT | 0.008 | 0.06 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141217 | `GC Thread#0` | GC | 0.008 | 0.06 | 0.000 | 0.000 | 0.009 |
| Binding 무제한 | target | 141389 | `GC Thread#3` | GC | 0.008 | 0.06 | 0.000 | 0.000 | 0.013 |
| Binding 무제한 | target | 141387 | `GC Thread#1` | GC | 0.007 | 0.06 | 0.000 | 0.000 | 0.014 |
| Binding 무제한 | target | 141390 | `GC Thread#4` | GC | 0.007 | 0.05 | 0.000 | 0.000 | 0.017 |
| Binding 무제한 | target | 141388 | `GC Thread#2` | GC | 0.007 | 0.05 | 0.000 | 0.000 | 0.020 |
| Binding 무제한 | target | 141393 | `GC Thread#7` | GC | 0.007 | 0.05 | 0.000 | 0.000 | 0.021 |
| Binding 무제한 | target | 141267 | `bench-stats` | JVM/기타 | 0.002 | 0.02 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141233 | `C1 CompilerThre` | JIT | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141226 | `VM Thread` | JVM/기타 | 0.001 | 0.01 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141220 | `G1 Refine#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.004 |
| Binding 무제한 | target | 141250 | `ZLINKbg/Reaper` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141254 | `ZLINKbg/IO/2` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141255 | `ZLINKbg/IO/3` | binding/Core I/O | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141260 | `HTTP-Dispatcher` | Framework dispatcher/handler carrier | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141262 | `bench-raw-comma` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141266 | `bench-stats` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141221 | `G1 Service` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141231 | `Monitor Deflati` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141230 | `Service Thread` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141225 | `VM Periodic Tas` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141259 | `idle-timeout-ta` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141235 | `Common-Cleaner` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141228 | `Finalizer` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141219 | `G1 Conc#0` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141218 | `G1 Main Marker` | GC | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141234 | `Notification Th` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141227 | `Reference Handl` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141229 | `Signal Dispatch` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141251 | `ZLINKbg/core-ct` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141203 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |
| Binding 무제한 | target | 141216 | `java` | JVM/기타 | 0.000 | 0.00 | 0.000 | 0.000 | 0.000 |

## 판정

이번 측정에서 Framework의 active 처리율은 57,549.8 req/s였다. 해당 속도에서는 source의 `bench-phase-active` 제출 thread가 0.714 core와 12.41 µs/request를 사용했고, target에서 가장 바쁜 thread는 `ZLINKbg/IO/1`로 0.773 core와 13.42 µs/request를 사용했다. 어느 thread도 실제 측정 속도에서 0.9 core에 도달하지 않았다. runnable 상태로 기다린 시간도 이 thread에서 0.07 µs/request였다. 다른 주요 source thread의 대기 시간은 0.09~0.32 µs/request였고 target의 주요 thread는 0.01~0.50 µs/request였다. OS run-queue 경쟁은 측정된 처리율의 제한 원인으로 보이지 않는다.

75,645 req/s는 제시된 unpaced binding 기준 126,075 req/s의 0.6배다. 이 목표에서 thread 하나가 1 core를 넘지 않으려면 13.22 µs/request 이하여야 한다. Framework target의 `ZLINKbg/IO/1`은 13.42 µs/request이므로 선형 환산 시 1.015 core가 필요하다. 이 thread가 가장 먼저 1 core 한계를 넘는다. 같은 역할의 paced binding target `ZLINKbg/IO/1`은 7.67 µs/request, 0.441 core였다. 차이는 Framework가 5.75 µs/request 더 크며, Framework 값은 binding의 1.75배다.

source 쪽도 목표 속도에서 여유가 작다. source `ZLINKbg/IO/2`는 Framework에서 13.02 µs/request이며, 75,645 req/s에서 0.985 core가 필요하다. paced binding의 같은 이름 thread는 6.43 µs/request와 0.370 core였다. source `ZLINKbg/IO/0`은 Framework 12.51 µs/request, binding 8.39 µs/request였다. Framework 제출 thread는 12.41 µs/request, binding 제출 thread는 8.19 µs/request였다. 따라서 Framework caller만을 병목으로 지목할 근거는 없다. target I/O thread 1과 source I/O thread 2가 더 높은 CPU 비용을 보였고 source mesh thread도 11.92 µs/request였다. 이 네 thread의 projected utilization은 목표 속도에서 약 0.90~1.02 core다.

따라서 가장 가까운 단일 제한은 target `ZLINKbg/IO/1`의 I/O 진행이며, 전체적으로는 Framework target I/O와 source I/O 및 mesh 처리의 CPU 비용이 함께 목표 속도에 근접한다. 현재 57,550 req/s에서 75,645 req/s까지는 18,095 req/s, 31.4%가 더 필요하다. target I/O thread 1은 목표에서 1 core 예산을 약 0.20 µs/request 초과한다. 측정된 per-request 비용이 그대로 유지되면 target I/O thread만으로도 75,645 req/s를 처리할 수 없다.

## 처리율과 in-flight

최종 Framework 실행은 10초 active window에서 575,498건을 완료해 57,549.8 req/s였다. 평균 latency는 10.739 ms, p99는 17.935 ms, peak in-flight는 1,758이었다. Framework request-backpressure에는 application in-flight cap이 설정되어 있지 않았다. 그러므로 이 수치에서 `latency × in-flight cap`으로 처리율을 설명할 수 없다. 이 실행에서 한계로 드러난 것은 고정 cap에 따른 순차 handoff 지연이 아니라, target/source의 단일 I/O thread별 요청당 CPU 요구량이다.

paced binding은 57,548.7 req/s로 Framework와 1.1 req/s 차이(0.002%)였다. 평균 latency는 0.101 ms, p99는 0.177 ms, peak in-flight는 451이었다. binding 무제한 실행은 128,818.8 req/s였고 평균 latency는 0.128 ms, p99는 1.130 ms, peak in-flight는 1,878이었다. 이번 unpaced binding의 0.6배는 77,291 req/s다. 본문에서 요청한 75,645 req/s 판정은 제공된 126,075 req/s baseline에 맞췄다.

## Framework 코드 표본

Framework 최종 실행의 source와 target JVM에 `settings=profile` JFR recording을 실행했다. source recording은 43초, `jdk.ExecutionSample`은 1,664건이었다. source `zlink-jvm-raw-mesh-bench` thread의 상위 Framework frame은 `ZLinkServiceOperationRegistry.take`(35 표본), `ZLinkJavaRawMeshNode.decodeRequestReply`(16 표본), `ZLinkServiceM6AWireCodec.decodeMultipartView`(18 표본)이었다. 확인 위치는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:516`, `.../binding/ZLinkJavaRawMeshNode.java:5293`, `.../internal/service/ZLinkServiceM6AWireCodec.java:534`다. 이 mesh thread는 11.92 µs/request와 0.686 core를 사용했다.

target recording에서 unnamed virtual thread sample의 상위 Framework frame은 `ZLinkSpringHandlerFactory.constructHandler`(278 표본)이었다. 위치는 `framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java:488`다. target `ZLINKbg/IO/1` 자체는 JFR Java execution sample에 나타나지 않았다. 따라서 이 Core I/O thread의 native poller 구간에 실행된 Framework Java method를 지정할 수는 없다. JFR의 Spring handler 표본은 target application 경로를 설명하지만 `ZLINKbg/IO/1`의 stack으로 해석하지 않았다.

## 측정 조건과 방법

세 최종 실행은 condition B로 target CPU 8–15, source CPU 0–7에 고정했고 `request-backpressure`, payload 1,024B, `RUNS=1`, 30초 warmup, 10초 active window로 실행했다. 각 최종 실행은 `/root/jobs/fx/cx/measure.lock`을 `flock -o`로 획득한 상태에서 진행했다. 빌드는 요청한 `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh`로 완료했다. paced binding 목표는 Framework 실측 처리율 57,550 req/s였다.

thread별 CPU runtime과 run-queue wait는 active window 앞뒤의 `/proc/<pid>/task/<tid>/schedstat` 차이를 사용했다. context switch는 같은 시각의 thread `status` 파일에서 voluntary와 nonvoluntary 값을 읽어 계산했다. active window 경계와 가장 가까운 sample은 window 시작에서 최대 31.2 ms, 종료에서 최대 11.1 ms 떨어져 있었다. CPU cores는 schedstat runtime을 10초 window로 나눈 값이고 µs/request 및 context switch/request는 process별 완료 또는 target 수신 건수를 분모로 했다. `/proc`의 comm은 15글자 제한으로 잘릴 수 있어 역할 분류에 JFR의 전체 thread name을 함께 대조했다.

초기 2초 warmup pilot과 두 번의 잘못된 pacing 실행은 최종 데이터에 포함하지 않았다. 첫 pacing 위치는 Java binding의 raw `runRaw` 경로에 적용되지 않아 117,974 req/s로 동작했다. 두 번째 실행에서는 `runRaw`로 위치를 옮겼지만 목표시각을 매회 재설정해 29,411 req/s에 그쳤다. 목표시각 누적을 적용한 최종 실행은 57,548.7 req/s로 목표와 0.002% 차이를 보였다.

## 변경 파일과 검증

`framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java`에서 `ZLINK_JAVA_BENCH_REQUEST_RATE`를 읽고 `zlink-java` raw request-backpressure submissions에만 pace를 적용했다. 기본값은 비활성이다. 기존 in-flight 동작은 유지했고 admission backpressure 외에 별도 cap을 추가하지 않았다.

`framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchOptions.java`에서 환경 변수의 양의 정수 형식을 확인한다. 최종 변경 뒤 Java build가 성공했다. 별도 test는 실행하지 않았다. 직접 편집한 worktree 파일은 이 두 bench 파일이다.

## 기준선 차이

제시된 기준선은 Framework 63,500 req/s, binding 126,075 req/s였다. 최종 Framework 측정 57,550 req/s는 기준선보다 9.4% 낮았고, 최종 binding 무제한 측정 128,819 req/s는 기준선보다 2.2% 높았다. 각 조건은 RUNS=1이므로 이 차이를 코드 변화와 환경 변화로 분리할 수 없다. 0.6 목표 판정은 본문에서 제공된 126,075 req/s 기준을 유지했다.
