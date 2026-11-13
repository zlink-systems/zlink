# Current C++ Framework vs binding stage profile — issue #3867

Cell: **request-backpressure, 1024 bytes**, `ee1139025782688e33109455041d8a0222cc8fe3`, 2026-11-05 KST. The Framework application submitter is the critical CPU resource. Its largest exclusive stage is allocation: **7.559 µs/message**, followed by Framework bookkeeping **5.057** and binding/Core calls **5.004**. Whole-source CPU is **103.093 µs/message**, versus binding **9.124**. These are current measurements; no earlier profile timings were reused.

**Candidate R from the previous static analysis is already implemented in #3600 (`c8274434c0`).** It cannot be credited again. Of the remaining named candidates, W has the largest observed critical-thread enclosing budget, **2.395 µs/message**; even deleting that entire budget gives only an optimistic ratio040 submit-CPU ceiling of about **0.193**, holding other resource costs fixed.

## Measurement and evidence

- [Preflight](preflight.txt) passed before the first benchmark. HEAD is detached at the supplied main commit. The four existing `run_local.sh` pinning changes were retained; no repository tracked file or existing binary was modified. Source CPUs 0–7; target CPUs 8–15.
- Every benchmark/profile/check run held `flock -o /root/jobs/fx/cx/measure.lock`. The runs were sequential and waited for other jobs. Existing Release binaries were used with `SKIP_BUILD=1`. Only the small allocation counter and its validation executable were built, wholly in this directory, through `cpp-build-slot.sh`, under a shared measure lock, with `cmake --build ... --parallel 3`.
- Job-local copies of the current runner used `PATTERNS=request-backpressure`, `PAYLOAD_SIZES=1024`, the two `IMPLEMENTATIONS`, `RUNS=1`, warmup 10 s, active 20 s, and job-local output/stamps. See [run.sh](run.sh), [instrumented runner](instrumented/run_local.sh), and [collector](collect.py). Warmup, payload, admission, reply validation, and target/source count checks remain the repository runner's behavior.
- `perf record -e cpu-clock -F 199 --call-graph dwarf,16384 --sample-cpu -p PID` attached separately to source and target only within active. Capture CPU/message snapshots span about 8 s. Recorder startup makes effective sample spans 6.93–7.51 s; each thread's stage mix is normalized to its CPU delta across the enclosing capture window. No off-CPU latency is being reported as CPU.
- Thread CPU/message = `/proc/PID/task/TID/schedstat` runtime delta / source completed-reply delta. Core share = thread runtime / capture elapsed time. Sequential HTTP/proc snapshots and schedstat updates cause boundary skew: binding submitter shows 1.016 cores, meaning approximately one saturated core, not capacity above one core. Full-phase benchmark CPU counters provide an independent comparison below.
- Every sample enters one exclusive stage. Stage µs = thread CPU/message × sampled-period fraction on that thread; whole-process values sum threads. Inclusive function/scope values overlap. Generic leaf functions inherit the nearest recognized caller. Exact removable cost is not measured by an enclosing scope.
- [Profile quality](quality.json): no empty or wholly unresolved stacks; unknown leaves are 1.01–1.89% of sampled weight and use resolved caller attribution. Perf stderr reports no lost samples. The Framework submitter has 1319 samples; one self sample is approximately 0.018 µs/message. Small function timings have sampling uncertainty. Binaries have symbols/unwind data but no complete source-line/inline timing; file locations below come from source matching.
- Heap calls use a job-local LD_PRELOAD interposer on external libc `malloc/calloc/realloc`, with per-thread counters and active-window deltas; `new` is counted when it reaches these APIs. `realloc` counts as one allocation call. Internal libc allocation entry points, aligned allocation APIs, direct mmap and custom pools are excluded. Free counts are separate and need not balance these intercepted allocation calls. The [positive/negative validation](counter-check/validation.json) observed exactly 1000 malloc, 1000 calloc, 1000 realloc and 2000 free calls and rejected a deliberately incorrect expectation.
- Futex/syscall/context-switch counts use per-thread `perf stat`, enabled/disabled with its control pipe during active, in separate counter runs. No malloc uprobes were used. Counter/snapshot windows have millisecond boundary skew. Call counts are not stack-attributed allocation counts. Perf emitted `<not counted>` for idle/auxiliary threads with zero counter-running time; those auxiliary call counts are **not measured**, rather than proven zero. Their enclosing proc-window CPU totals are 0.33/33.53 ms for Framework source/target and 0.37/156.30 ms for binding source/target; the nontrivial target entries are snapshot/stats auxiliary threads. All workload submit/receive/I/O/dispatch/completion workers have valid counters. Whole-process call tables sum valid thread counters and are lower bounds by these auxiliary omissions.
- All six final benchmark cells passed count validation with zero errors, abandoned requests, header-validation failures, or contamination. MemAvailable was above 12 GiB at every capture; raw profile files are all below 1 GB. No frame-pointer rebuild, implementation change, sub-agent, other model, CI, or unrelated test run was used.

## Throughput and profiling overhead

| Measurement | Framework msg/s | Binding msg/s | F/B |
|---|---|---|---|
| ratio040, three-run medians | 36,315 | 220,467 | 0.165 |
| New unprofiled, whole 20 s | 42,890 | 270,366 | 0.159 |
| CPU profile, whole 20 s | 42,397 | 271,026 | 0.156 |
| Counters, whole 20 s | 41,199 | 257,067 | 0.160 |
| Unprofiled capture window | 43,462 | 285,888 | 0.152 |
| CPU-profile capture window | 41,300 | 271,112 | 0.152 |
| Counter capture window | 40,438 | 258,380 | 0.157 |

New unprofiled rates differ from ratio040 by **+18.1% Framework / +22.6% binding**. These runs use longer warmup/active windows than ratio040. One new unprofiled run per implementation was measured; run-to-run variance and a causal explanation for the baseline change are **not measured**. New results do not replace the supplied ratio040 values.

| CPU µs/message | F submit | B submit | F source | B source | F target | B target |
|---|---|---|---|---|---|---|
| ratio040 medians | 26.138 | 4.533 | 107.967 | 10.749 | 82.943 | 10.997 |
| New unprofiled full-phase counters | 22.385 | 3.696 | 94.213 | 8.948 | 71.673 | 8.912 |

CPU-profile capture-window rates are about 5.0% lower for Framework and 5.2% lower for binding than their unprofiled capture windows; counter-window rates are about 7.0% / 9.6% lower. These differences include instrumentation and unmeasured run variation; they are not isolated overhead estimates.

## Per-thread CPU

**Critical thread: Framework source application submitter TID 1107480**, `framework_driver_t::run_tasks<BenchPayload>`, 23.405 µs/message and 0.967 cores. Binding is also limited by its application submitter, TID 1108879, 3.746 µs/message, approximately one core. The Framework target's busiest thread is Core IO/1; no target thread reaches one core.

Every thread is listed below. Numbered workers are paired by role and TID order, not by a cross-run thread identity. Auxiliary/idle means its specific role was not resolved from sampled stacks. TID and comm, CPU deltas, context switches, and stage values are also preserved in each capture's `*.threads.csv`.

### Source

| Thread role | F TID | F µs/msg | F cores | B TID | B µs/msg | B cores |
|---|---|---|---|---|---|---|
| application submitter | 1107480 | 23.405 | 0.967 | 1108879 | 3.746 | 1.016 |
| Framework receive role | 1106958 | 18.350 | 0.758 | — | — | — |
| Core IO/0 01 | 1106933 | 0.004 | 0.000 | 1108286 | 0.001 | 0.000 |
| Core IO/0 02 | 1106953 | 19.494 | 0.805 | 1108305 | 2.513 | 0.681 |
| Core IO/1 01 | 1106934 | 0.004 | 0.000 | 1108287 | 0.001 | 0.000 |
| Core IO/1 02 | 1106954 | 0.004 | 0.000 | 1108306 | 0.001 | 0.000 |
| Core IO/2 01 | 1106935 | 0.004 | 0.000 | 1108288 | 0.001 | 0.000 |
| Core IO/2 02 | 1106955 | 15.247 | 0.630 | 1108307 | 2.819 | 0.764 |
| Core IO/3 01 | 1106936 | 0.004 | 0.000 | 1108289 | 0.001 | 0.000 |
| Core IO/3 02 | 1106956 | 0.004 | 0.000 | 1108308 | 0.001 | 0.000 |
| completion worker 01 | 1106922 | 3.238 | 0.134 | — | — | — |
| completion worker 02 | 1106923 | 3.246 | 0.134 | — | — | — |
| completion worker 03 | 1106924 | 3.264 | 0.135 | — | — | — |
| completion worker 04 | 1106925 | 3.255 | 0.134 | — | — | — |
| completion worker 05 | 1106926 | 3.248 | 0.134 | — | — | — |
| completion worker 06 | 1106927 | 3.238 | 0.134 | — | — | — |
| completion worker 07 | 1106928 | 3.250 | 0.134 | — | — | — |
| completion worker 08 | 1106929 | 3.245 | 0.134 | — | — | — |
| stats HTTP worker | — | — | — | 1108275 | 0.040 | 0.011 |
| stats HTTP worker 01 | 1106921 | 0.228 | 0.009 | — | — | — |
| stats HTTP worker 02 | 1106950 | 0.228 | 0.009 | — | — | — |
| auxiliary/idle 01 | 1106910 | 0.003 | 0.000 | 1108263 | 0.001 | 0.000 |
| auxiliary/idle 02 | 1106913 | 0.008 | 0.000 | 1108266 | 0.000 | 0.000 |
| auxiliary/idle 03 | 1106914 | 0.010 | 0.000 | 1108267 | 0.000 | 0.000 |
| auxiliary/idle 04 | 1106915 | 0.011 | 0.000 | 1108268 | 0.000 | 0.000 |
| auxiliary/idle 05 | 1106916 | 0.010 | 0.000 | 1108269 | 0.000 | 0.000 |
| auxiliary/idle 06 | 1106917 | 0.006 | 0.000 | 1108270 | 0.000 | 0.000 |
| auxiliary/idle 07 | 1106918 | 0.010 | 0.000 | 1108271 | 0.000 | 0.000 |
| auxiliary/idle 08 | 1106919 | 0.016 | 0.001 | 1108272 | 0.000 | 0.000 |
| auxiliary/idle 09 | 1106920 | 0.011 | 0.000 | 1108273 | 0.000 | 0.000 |
| auxiliary/idle 10 | 1106931 | 0.004 | 0.000 | 1108276 | 0.000 | 0.000 |
| auxiliary/idle 11 | 1106932 | 0.000 | 0.000 | 1108277 | 0.000 | 0.000 |
| auxiliary/idle 12 | 1106937 | 0.002 | 0.000 | 1108278 | 0.000 | 0.000 |
| auxiliary/idle 13 | 1106938 | 0.003 | 0.000 | 1108279 | 0.000 | 0.000 |
| auxiliary/idle 14 | 1106939 | 0.003 | 0.000 | 1108280 | 0.000 | 0.000 |
| auxiliary/idle 15 | 1106940 | 0.003 | 0.000 | 1108281 | 0.000 | 0.000 |
| auxiliary/idle 16 | 1106941 | 0.003 | 0.000 | 1108282 | 0.000 | 0.000 |
| auxiliary/idle 17 | 1106942 | 0.003 | 0.000 | 1108283 | 0.000 | 0.000 |
| auxiliary/idle 18 | 1106943 | 0.003 | 0.000 | 1108284 | 0.001 | 0.000 |
| auxiliary/idle 19 | 1106944 | 0.003 | 0.000 | 1108285 | 0.000 | 0.000 |
| auxiliary/idle 20 | 1106945 | 0.000 | 0.000 | 1108290 | 0.000 | 0.000 |
| auxiliary/idle 21 | 1106946 | 0.000 | 0.000 | 1108291 | 0.000 | 0.000 |
| auxiliary/idle 22 | 1106947 | 0.000 | 0.000 | 1108292 | 0.000 | 0.000 |
| auxiliary/idle 23 | 1106948 | 0.001 | 0.000 | 1108293 | 0.000 | 0.000 |
| auxiliary/idle 24 | 1106951 | 0.004 | 0.000 | 1108294 | 0.000 | 0.000 |
| auxiliary/idle 25 | 1106952 | 0.000 | 0.000 | 1108295 | 0.000 | 0.000 |
| auxiliary/idle 26 | 1106957 | 0.004 | 0.000 | 1108296 | 0.000 | 0.000 |
| auxiliary/idle 27 | 1106960 | 0.000 | 0.000 | 1108297 | 0.000 | 0.000 |
| auxiliary/idle 28 | 1106961 | 0.012 | 0.000 | 1108298 | 0.000 | 0.000 |
| auxiliary/idle 29 | 1106962 | 0.003 | 0.000 | 1108299 | 0.000 | 0.000 |
| auxiliary/idle 30 | 1106963 | 0.000 | 0.000 | 1108300 | 0.000 | 0.000 |
| auxiliary/idle 31 | — | — | — | 1108301 | 0.000 | 0.000 |
| auxiliary/idle 32 | — | — | — | 1108303 | 0.001 | 0.000 |
| auxiliary/idle 33 | — | — | — | 1108304 | 0.000 | 0.000 |
| auxiliary/idle 34 | — | — | — | 1108309 | 0.000 | 0.000 |

### Target

| Thread role | F TID | F µs/msg | F cores | B TID | B µs/msg | B cores |
|---|---|---|---|---|---|---|
| Framework receive role | 1106889 | 16.986 | 0.702 | — | — | — |
| Core IO/0 | 1106884 | 15.533 | 0.642 | — | — | — |
| Core IO/0 01 | — | — | — | 1108237 | 0.001 | 0.000 |
| Core IO/0 02 | — | — | — | 1108253 | 2.198 | 0.596 |
| Core IO/1 | 1106885 | 18.884 | 0.780 | — | — | — |
| Core IO/1 01 | — | — | — | 1108238 | 0.001 | 0.000 |
| Core IO/1 02 | — | — | — | 1108254 | 3.312 | 0.898 |
| Core IO/2 | 1106886 | 0.004 | 0.000 | — | — | — |
| Core IO/2 01 | — | — | — | 1108239 | 0.001 | 0.000 |
| Core IO/2 02 | — | — | — | 1108255 | 0.001 | 0.000 |
| Core IO/3 | 1106887 | 0.005 | 0.000 | — | — | — |
| Core IO/3 01 | — | — | — | 1108240 | 0.001 | 0.000 |
| Core IO/3 02 | — | — | — | 1108256 | 0.001 | 0.000 |
| request handler loop | — | — | — | 1108257 | 3.261 | 0.884 |
| application dispatch worker 01 | 1107183 | 11.047 | 0.456 | — | — | — |
| application dispatch worker 02 | 1107482 | 11.716 | 0.484 | — | — | — |
| stats HTTP worker 01 | 1106873 | 0.234 | 0.010 | 1108226 | 0.033 | 0.009 |
| stats HTTP worker 02 | 1106890 | 0.097 | 0.004 | 1108248 | 0.072 | 0.020 |
| auxiliary/idle 01 | 1106862 | 0.003 | 0.000 | 1108215 | 0.000 | 0.000 |
| auxiliary/idle 02 | 1106865 | 0.000 | 0.000 | 1108218 | 0.000 | 0.000 |
| auxiliary/idle 03 | 1106866 | 0.000 | 0.000 | 1108219 | 0.000 | 0.000 |
| auxiliary/idle 04 | 1106867 | 0.000 | 0.000 | 1108220 | 0.000 | 0.000 |
| auxiliary/idle 05 | 1106868 | 0.000 | 0.000 | 1108221 | 0.000 | 0.000 |
| auxiliary/idle 06 | 1106869 | 0.000 | 0.000 | 1108222 | 0.000 | 0.000 |
| auxiliary/idle 07 | 1106870 | 0.000 | 0.000 | 1108223 | 0.000 | 0.000 |
| auxiliary/idle 08 | 1106871 | 0.000 | 0.000 | 1108224 | 0.000 | 0.000 |
| auxiliary/idle 09 | 1106872 | 0.000 | 0.000 | 1108225 | 0.000 | 0.000 |
| auxiliary/idle 10 | 1106874 | 0.000 | 0.000 | 1108227 | 0.000 | 0.000 |
| auxiliary/idle 11 | 1106875 | 0.000 | 0.000 | 1108228 | 0.000 | 0.000 |
| auxiliary/idle 12 | 1106876 | 0.000 | 0.000 | 1108229 | 0.000 | 0.000 |
| auxiliary/idle 13 | 1106877 | 0.000 | 0.000 | 1108230 | 0.000 | 0.000 |
| auxiliary/idle 14 | 1106878 | 0.000 | 0.000 | 1108231 | 0.000 | 0.000 |
| auxiliary/idle 15 | 1106879 | 0.000 | 0.000 | 1108232 | 0.000 | 0.000 |
| auxiliary/idle 16 | 1106880 | 0.000 | 0.000 | 1108233 | 0.000 | 0.000 |
| auxiliary/idle 17 | 1106881 | 0.000 | 0.000 | 1108234 | 0.000 | 0.000 |
| auxiliary/idle 18 | 1106882 | 0.005 | 0.000 | 1108235 | 0.001 | 0.000 |
| auxiliary/idle 19 | 1106883 | 0.000 | 0.000 | 1108236 | 0.000 | 0.000 |
| auxiliary/idle 20 | 1106891 | 0.000 | 0.000 | 1108241 | 0.000 | 0.000 |
| auxiliary/idle 21 | 1106892 | 0.000 | 0.000 | 1108242 | 0.000 | 0.000 |
| auxiliary/idle 22 | 1106893 | 0.000 | 0.000 | 1108243 | 0.000 | 0.000 |
| auxiliary/idle 23 | 1106894 | 0.000 | 0.000 | 1108244 | 0.000 | 0.000 |
| auxiliary/idle 24 | 1106895 | 0.000 | 0.000 | 1108245 | 0.000 | 0.000 |
| auxiliary/idle 25 | 1106896 | 0.000 | 0.000 | 1108246 | 0.000 | 0.000 |
| auxiliary/idle 26 | 1106897 | 0.000 | 0.000 | 1108247 | 0.000 | 0.000 |
| auxiliary/idle 27 | 1106900 | 0.000 | 0.000 | 1108250 | 0.000 | 0.000 |
| auxiliary/idle 28 | 1106901 | 0.015 | 0.001 | 1108251 | 0.001 | 0.000 |
| auxiliary/idle 29 | 1106902 | 0.000 | 0.000 | 1108252 | 0.000 | 0.000 |
| auxiliary/idle 30 | 1106909 | 0.001 | 0.000 | 1108258 | 0.000 | 0.000 |
| auxiliary/idle 31 | — | — | — | 1108262 | 0.000 | 0.000 |

## Exclusive stage CPU

Allocation and synchronization have precedence over caller ownership: allocator locks stay in allocation; other futex/mutex/CV/scheduler work stays in handoff/wake. Thus the Core column excludes native allocation and lock work already charged elsewhere. This is a global cost taxonomy, not a claim that every lock is owned by Framework.

| Stage | Function-to-stage mapping |
|---|---|
| Allocation | libc malloc/free/calloc/realloc, tcache, `_int_malloc/_int_free`, new/delete, allocator arena/kernel descendants. C++ has no GC here. |
| Lane/worker handoff and wake | futex, pthread mutex/condition-variable and scheduler paths; `state_lane_t`, serial execution queues, offload queues, completion dispatcher scheduling. Native locks are included here except allocator locks. |
| Task/coroutine/result state | Framework task/shared-state/completion-source/request receiver/access, observe/await/result and coroutine lifetime work; standard promise/future machinery. Allocation and locks excluded. |
| Codec | protobuf/generated BenchPayload serialization; envelope/client-call codec and deadline text formatting; serializer lookup; service-wire encode/decode and byte/container operations. Allocation excluded. |
| Binding/Core | C++ binding, libzlink, native transport/Asio and kernel send/recv/read/write/epoll descendants. Framework completion-pool Asio is handoff, not transport. |
| Framework bookkeeping | Remaining registry/admission/deadline registration, transport selection, host dispatch/scope/context, references/callback lifetime, and lifecycle work. |
| Benchmark driver | Source submit/validate/latency loop, ready/completion-signal non-lock work, stats/metrics handlers. |
| Other | No usable recognized owner, or unsampled auxiliary CPU. No fabricated stage attribution. |

| µs/message | F critical | B critical | F source | B source | F target | B target |
|---|---|---|---|---|---|---|
| allocation | 7.559 | 0.923 | 15.928 | 1.416 | 5.737 | 0.833 |
| handoff/wake | 1.473 | 0.200 | 15.342 | 0.274 | 6.106 | 0.205 |
| task/coroutine/future | 0.781 | 0.000 | 2.095 | 0.000 | 0.273 | 0.000 |
| codec | 2.626 | 0.126 | 7.837 | 0.126 | 4.683 | 0.293 |
| binding/Core | 5.004 | 2.117 | 45.763 | 6.889 | 46.481 | 7.370 |
| Framework bookkeeping | 5.057 | 0.000 | 15.066 | 0.033 | 11.032 | 0.033 |
| benchmark driver | 0.905 | 0.377 | 0.905 | 0.377 | 0.186 | 0.140 |
| other | 0.000 | 0.003 | 0.157 | 0.009 | 0.033 | 0.006 |
| **Total** | 23.405 | 3.746 | 103.093 | 9.124 | 74.531 | 8.882 |

The Framework source spends **79.688 µs/message outside the submitter**. Its eight completion workers together use 25.984 µs/message. These costs cannot be added to submitter CPU as wall-time savings. The pool is already sized from process-available CPUs in `C/dispatch/provider_task_context.cpp:56` / #3557; the obsolete 16-worker observation is not current evidence.

## Counts per completed message

Separate counter runs, using same-window completed replies as denominator. Heap allocations = intercepted malloc + calloc + realloc. Syscalls include futex; columns are not additive. Critical source TIDs differ from the CPU-profile run.

| Scope | F switches | F futex | F heap calls | F syscalls | B switches | B futex | B heap calls | B syscalls |
|---|---|---|---|---|---|---|---|---|
| critical source | 0.0360 | 1.6673 | 49.333 | 2.6579 | 0.0002 | 0.4284 | 12.395 | 0.5499 |
| whole source | 2.4606 | 9.3719 | 65.074 | 18.4747 | 0.1103 | 1.0866 | 12.967 | 1.9966 |
| whole target | 1.3143 | 2.0399 | 49.559 | 10.2878 | 0.0899 | 0.5337 | 7.728 | 1.4103 |
| both processes | 3.7749 | 11.4117 | 114.633 | 28.7625 | 0.2002 | 1.6203 | 20.694 | 3.4069 |

| Syscall/message | F source | B source | F target | B target |
|---|---|---|---|---|
| sendto | 0.8833 | 0.1171 | 0.9113 | 0.1830 |
| recvfrom | 1.1370 | 0.3075 | 1.1733 | 0.2444 |
| read | 1.4215 | 0.0310 | 1.5239 | 0.0000 |
| write | 0.5887 | 0.0155 | 0.4292 | 0.0000 |
| epoll_wait | 1.9353 | 0.3452 | 2.0302 | 0.4052 |

The binding benchmark also hosts its stats HTTP service with Framework `app_t::run`; its tiny off-path bookkeeping samples come from that host, not request processing. Raw counters and per-thread totals: [counts-summary.json](counts-summary.json). Calls through uninstrumented allocation APIs and per-allocation-site counts are **not measured**. These counts are observations under counter overhead, not literal construction counts inferred from source.

## Top 20 Framework functions on the critical thread

Ordered by self CPU, then inclusive CPU. Inclusive times overlap and must not be summed. Generated protobuf and benchmark functions, libstdc++ and allocator functions are excluded from this Framework-only list. Full symbols, including lambdas/templates, are in [top20.json](top20.json) and the source critical-functions CSV. C = `framework/languages/cpp/framework/src/runtime/`; P = `framework/languages/cpp/framework/include/zlink/framework/contracts/`.

| # | File:function | Self µs/msg | Inclusive µs/msg |
|---|---|---|---|
| 1 | `C/messaging/envelope_codec.cpp:header_writer_t::append` | 0.284 | 0.532 |
| 2 | `C/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased` | 0.213 | 19.040 |
| 3 | `C/mesh/raw_mesh_node_owner.cpp:raw_mesh_node_owner_t::request_with_header · lane lambda #1` | 0.213 | 10.700 |
| 4 | `C/messaging/envelope_codec.cpp:header_writer_t::string` | 0.160 | 0.231 |
| 5 | `C/codecs/serializer.cpp:serializer_registry_t::cached_serializer` | 0.142 | 0.213 |
| 6 | `C/foundation/operation_registry.cpp:operation_registry_state_t::register_operation` | 0.124 | 0.958 |
| 7 | `C/mesh/raw_mesh_node_owner.cpp:raw_mesh_node_owner_t::request_with_header` | 0.106 | 11.001 |
| 8 | `C/backend/raw_route_port.cpp:raw_route_port_t::request · callback overload` | 0.106 | 7.080 |
| 9 | `P/channels/channel.hpp:route_client_t::request_to_node<BenchPayload>` | 0.106 | 0.302 |
| 10 | `P/dispatch/request_task.hpp:request_task_access_t::receive<BenchPayload>` | 0.089 | 20.229 |
| 11 | `P/dispatch/task.hpp:task_completion_source_t<void>::complete · isra clone` | 0.089 | 0.373 |
| 12 | `P/dispatch/task.hpp:task_wait_registration_t::prepare_context` | 0.089 | 0.213 |
| 13 | `P/dispatch/task.hpp:ambient_context_scope_t::~ambient_context_scope_t` | 0.089 | 0.106 |
| 14 | `C/dispatch/coroutine_executor.cpp:capture_native_scheduler` | 0.089 | 0.089 |
| 15 | `C/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased · encoded-result projection lambda` | 0.089 | 0.089 |
| 16 | `C/protocol/service_wire_codec.cpp:append_text8` | 0.089 | 0.089 |
| 17 | `C/messaging/envelope_codec.cpp:envelope_codec_t::encode_header` | 0.071 | 0.940 |
| 18 | `P/dispatch/task.hpp:task_shared_state_t<BenchPayload>::on_completed` | 0.071 | 0.621 |
| 19 | `C/mesh/service_topology_registry.cpp:service_topology_registry_t::peer` | 0.071 | 0.408 |
| 20 | `C/protocol/service_wire_codec.cpp:append_u32` | 0.071 | 0.142 |

## Re-ranked R / W / T / M / H

R's old intermediate raw-result task/producer/holder and host-result conversion path were removed from this benchmark's hot path by #3600. Current `C/mesh/raw_mesh_node_owner.cpp:248` calls the callback overload at `C/backend/raw_route_port.cpp:313`; `C/mesh/mesh_node_runtime.cpp:4396` submits directly into the prepared receiver. The task-returning overload and `compose_node_request_reply` still exist for other paths; their existence is not evidence that the benchmark pays for them. Required public reply/admission state remains.

Ratio040: F=36314.766, B=220467.251; submit CPU C=26.138 µs/message. Profile critical CPU is 23.405. For comparison only, a measured profile scope d is scaled by **C/profileCPU=1.1168**. The ratio ceiling is **U = 10^6 / [B × (C − scaled d)]**, holding other resource costs fixed. This is a submit-CPU capacity bound, not predicted throughput; at d=0 it is 0.174, already above observed F/B=0.165 because the submitter is not on CPU for the entire interval. The new-run column uses the new unprofiled C and B with the same stage-mix assumption. Exact candidate savings and post-change rates are **not measured**.

W/H/T/M selectors are enclosing budgets and retain necessary codec, public-state, invocation or reply work. Thus each is at most the reported budget, often less. Zero critical µs for off-thread work is structural for these candidates; indirect contention benefits are not measured. Candidates overlap and cannot be added.

| Rank / candidate | Stage / scope | Profile critical max µs | Scaled to ratio040 µs | U ratio040 | U new run | Whole source / target scope µs |
|---|---|---|---|---|---|---|
| 1. W | Service-wire aggregation/holder/multipart ownership, native materialization and destruction; required serialization/storage included in ceiling. | 2.395 | 2.675 | 0.193 | 0.184 | 2.928 / 1.552 |
| 2. H | Driver task observation, context capture, signal/queue/locks; includes required reply notification, so ready-admission-only saving is smaller. | 1.118 | 1.248 | 0.182 | 0.174 | 1.118 / 0.000 |
| 3. T | Target invocation coroutine/result/terminal lifetime; required handler/filter/transport work excluded by selector. | 0.000 | 0.000 | 0.174 | 0.165 | 0.000 / 3.552 |
| 4. M | Outer receive-activity coroutine/result/lifetime, not management/deadline work. | 0.000 | 0.000 | 0.174 | 0.165 | 0.240 / 0.239 |
| 5. R | Already implemented by #3600; original removable intermediate states/holder are absent from the measured hot path. | 0.000 | 0.000 | 0.174 | 0.165 | 0.000 / 0.000 |

The legacy script's broad R selector still tags **3.584 critical µs** (6.842 whole-source) of remaining public tasks, callback storage and raw-request parent work. That is **not** the old removable R design. Even treating all of this unrelated residual scope as removable gives only a 0.205 ratio040 CPU ceiling; no new representation proposal or achievable saving was measured. See [candidate-scopes.json](candidate-scopes.json) and selector comments in [candidates.py](candidates.py).

T has more whole-target scope than M, but both have zero direct submitter saving. Their order is secondary to W/H for this cell. M's nonzero whole-process measurement is not a per-message management-start count; its invocation frequency is **not measured**.

## Costs outside the earlier candidate list

| Cost / scope | File:function | Critical µs/msg | Whole source µs/msg |
|---|---|---|---|
| Native message-buffer allocation (subset of allocation) | `core/src/runtime/core/msg.cpp:msg_t::init_size; framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:serialize_protobuf` | 1.863 | 1.863 |
| Core Asio mailbox/reactor wake: epoll_ctl path (inclusive, overlaps Core/handoff) | `core/src/runtime/core/mailbox.cpp:mailbox_t::schedule_if_needed_unlocked → Asio → libc/kernel epoll_ctl` | 0.515 | 0.515 |
| Continuation context/scheduler capture (inclusive, overlaps tasks/bookkeeping/allocation) | `C/dispatch/coroutine_executor.cpp:capture_native_scheduler; P/dispatch/task.hpp:task_wait_registration_t::prepare_context` | 0.213 | 0.213 |
| Allocator lock/wake paths (subset of allocation; overlaps other allocation scopes) | `libc malloc/free → futex/scheduler` | 2.644 | 5.308 |
| UTC deadline formatting, libc strftime self only | `C/messaging/client_call_codec.cpp:format_utc_deadline` | 0.373 | 0.373 |

- **Transport and completion work is a large separate CPU constraint.** Source Core IO/0 and IO/2 together cost 34.749 µs/message. Target IO/0 and IO/1 cost 34.417 µs/message. Paths include `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:completion_owner_t::start_async_request / completion_entry_t::submit_request_attempt` and Core transport. The much higher sendto/recvfrom/read/epoll activity is measured; whether faster submission would improve batching is **not measured**. Core decisions remain in Core/binding.
- **Completion pool work remains after its affinity sizing fix.** Eight workers at `C/dispatch/provider_task_context.cpp:56` each consume about 3.24–3.26 µs/message. This is required-turn delivery plus callback/allocator/scheduler work, not proof that deleting a fresh completion turn or changing worker count is valid.
- **Public projection and ambient-context work remains after R.** See `P/dispatch/request_task.hpp:receive/receiver`, `P/dispatch/task.hpp:on_completed/prepare_context`, and `C/dispatch/coroutine_executor.cpp:capture_native_scheduler`. The source still intercepts about 49.3 allocation calls/message on the submitter. Removing historical internal result states does not remove these public/lifetime/codec/native allocations.
- **Deadline formatting is visible independently of result ownership.** `C/messaging/client_call_codec.cpp:format_utc_deadline` calls `gmtime_r/strftime`; `__strftime_internal` alone accounts for about 0.337 critical µs/message. This does not establish that the required wire deadline may be omitted.

## Limits and verification

Exact savings, an implementation A/B experiment, site-level allocation counts, admission ready/pending fraction, correlated per-message hop counts, off-CPU waits, and a full source-line/inline profile are **not measured**. The 4096-byte cell is outside this task and was **not measured**. Snapshot/sampling uncertainty and the baseline-rate difference limit the precision of all scaled ceilings.

Repository changes: none. Runtime rule/decision count before → after: unchanged. No new state/queue/lock was added. Implementation/refactoring audit: not performed in this evidence-only task. Final workload failures: none. 

## Gap to 0.6 and first change

At ratio040, reaching 0.6 requires 132,280 Framework msg/s: effective spacing must fall from **27.537 to 7.560 µs/message**, a gap of **19.977 µs/message**. The table scales the measured critical-thread mix to ratio040's submit CPU and expresses each entire stage budget as a fraction of that gap. These are maximum cost budgets, not fully removable overhead; they include work also required by binding.

There is also an aggregate CPU requirement: with eight CPUs per endpoint, the ratio040 goal allows **60.478 CPU µs/message per process**. Ratio040 source/target totals are 107.967/82.943; they exceed that budget by 47.489/22.465 µs/message. Improving submission alone therefore leaves whole-process work to reduce. This is a separate CPU-capacity constraint, not extra critical-path latency.

| Critical stage | Profile µs/msg | Scaled ratio040 µs/msg | Fraction of gap |
|---|---|---|---|
| allocation | 7.559 | 8.442 | 42.3% |
| Framework bookkeeping | 5.057 | 5.648 | 28.3% |
| binding/Core | 5.004 | 5.588 | 28.0% |
| codec | 2.626 | 2.933 | 14.7% |
| handoff/wake | 1.473 | 1.645 | 8.2% |
| benchmark driver | 0.905 | 1.011 | 5.1% |
| task/coroutine/future | 0.781 | 0.872 | 4.4% |

The named candidates' observed ceilings cover only **13.4% of the gap for W** and **6.2% for H**; T/M contribute no direct submitter time and the original R is already spent. The allocation stage represents **42.3%** of the required gap, bookkeeping **28.3%**, binding/Core **28.0%**, and codec **14.7%**. No single observed stage establishes a path to 0.6.

**First change I would implement: W, a service-wire writer that writes into the final native message buffer and consumes moved multipart ownership directly**, in `C/protocol/service_wire_codec.cpp:encode_application_payload`, using `C/backend/raw_binding_adapter.hpp:materialize_binding_part` as the current conversion boundary. It targets the largest remaining named critical-thread scope and allocation/ownership duplication without changing admission, completion, routing or wire semantics. Keep the existing public serializer path. Preserve required bytes and lifetimes; measure the change against the current unprofiled cell before attributing any gain. Its measured scope is an upper bound, and by itself it is far short of 0.6.
