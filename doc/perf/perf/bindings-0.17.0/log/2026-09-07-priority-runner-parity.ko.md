# 우선 4개 언어(C++·.NET·Java·Node) 러너 정책 정합 pass — 2026-09-07

> 범위: 직전 pass(`2026-09-07-multi-runner-policy-pass.ko.md` §6·§8)의 잔여분 중
> **우선 4개 언어**(D-BP8) 항목 A~F.
> `doc/perf/*.md` 정책 문서, 계획서, `decisions.ko.md`, `framework/**`, `core/**`,
> `bindings/c/perf/**`, `bindings/cpp/src`·`include`, `bindings/{go,rust,python}/**`는
> **수정하지 않았다**.
> **perf 러너는 한 번도 실행하지 않았다.** 빌드·컴파일·타입체크·contract test만 수행했다.
> git commit/push 하지 않았다.

---

## 0. 요약

| # | 항목 | C++ | .NET | Java | Node |
|---|---|---|---|---|---|
| A | multi/single metric header 시간원 monotonic | **수정** | **수정** | 이미 준수(주석만 추가) | **수정** |
| F | 메시지당 `getenv` 캐시 | **수정** | **수정** | 이미 준수(변경 없음) | **수정** |
| B | multi `PERF_MULTI_REQREP_MAX_OUTSTANDING` | 완료(직전 pass) | **수정** | 완료(직전 pass) | **수정** |
| C | SENDSEND server stdin STOP | 해당 없음 | **수정** | 해당 없음 | 해당 없음 |
| D | single REQREP 실행 모델(§1.1.2/§1.1.3) | **재구성** | **상한 추가**(이미 연속 제출) | **재구성** | **재구성** |
| D | single 7 pattern 등록 | 이미 7개 | 이미 7개 | 이미 7개 | 이미 7개 |
| E | 측정 anchor 6종 대조 | §5 표 | 〃 | 〃 | 〃 |

---

## 1. 작업 A — metric header·deadline 시간원을 monotonic으로

근거: `PERF_POLICY.md:127-141`
("시간원은 monotonic clock 하나로 고정한다. 모든 러너의 경과 시간, active deadline,
timeout, drain 한도, 그리고 metric header의 `sent_ts_ns`와 수신 판정 시각은 monotonic
시간원에서 읽는다", "같은 호스트에서 함께 실행되는 perf 프로세스들이 **같은 기준점을
공유하는** monotonic 시간원이어야 한다"), D-095.

기준 구현: C `bindings/c/perf/multi/common/perf_multi_metric_header.hpp:38-45`,
`bindings/c/perf/single/common/perf_single_metric_header.hpp:35-42` — `std::chrono::steady_clock`.

### 1.1 언어별 선택 시간원과 "cross-process 공유 monotonic" 근거

**실측으로 확인했다.** 같은 호스트에서 네 언어의 프로세스를 연달아 실행해 각 언어의
monotonic API 값과 C의 `clock_gettime(CLOCK_MONOTONIC)` / `steady_clock` 값을 비교했다.
네 값이 모두 **같은 boot 기준 축** 위에 있고, 차이는 프로세스 기동 시간(수 ms)뿐이다.

```
CLOCK_MONOTONIC      225126929098287     (C 프로그램)
steady_clock         225126929099058     (C++ = CLOCK_MONOTONIC)
java  System.nanoTime 225127275541838    (+0.35 ms = 프로세스 기동 간격)
node  hrtime.bigint   225127317133442    (+0.39 ms)
dotnet GetTimestamp   225141179141365    (같은 축, Frequency = 1_000_000_000)
```

| 언어 | 선택 API | 근거 |
|---|---|---|
| C(기준) | `std::chrono::steady_clock` | libstdc++에서 `clock_gettime(CLOCK_MONOTONIC)`. epoch = boot |
| C++ | `std::chrono::steady_clock` | 위와 동일한 표준 라이브러리·같은 호스트 |
| .NET | `System.Diagnostics.Stopwatch.GetTimestamp()` | .NET의 monotonic 시간원. 이 호스트에서 `Stopwatch.Frequency == 1_000_000_000`이고 값이 `CLOCK_MONOTONIC`과 동일 축(위 실측). Unix 구현이 `clock_gettime(CLOCK_MONOTONIC)`이므로 epoch = boot → 프로세스 간 공유 |
| Java | `System.nanoTime()` | JDK monotonic 시간원. HotSpot/Linux 구현이 `clock_gettime(CLOCK_MONOTONIC)`이고 값이 동일 축(위 실측). **주의**: javadoc은 "임의 원점"만 보장하고 프로세스 간 비교를 규격으로 보장하지 않는다. 이 호스트에서 조건을 만족한다는 것은 **구현 수준의 사실**이다(§8-1) |
| Node | `process.hrtime.bigint()` | libuv `uv_hrtime()` = Linux `clock_gettime(CLOCK_MONOTONIC)`. 값이 동일 축(위 실측). 문서는 "clock drift에 영향받지 않는, 과거 임의 시점 기준"만 명시 |

**조건을 만족하는 공개 시간원이 없는 언어는 4개 중 없었다.**

### 1.2 변경

| 언어 | 파일:줄 | before | after |
|---|---|---|---|
| C++ | `bindings/cpp/perf/multi/common/perf_metric_header.hpp:64` | `system_clock`(wall) | `steady_clock` |
| C++ | `bindings/cpp/perf/single/common/perf_single_metric_header.hpp:64` | `system_clock`(wall) | `steady_clock` |
| .NET | `bindings/dotnet/perf/common/Zlink.BindingBench.Common/PerfShared.cs:25-64` | `Stopwatch`를 `DateTime.UtcNow` epoch에 고정(**wall 기준점**), `double` 스케일 | wall 앵커 제거. `EpochNs`/`EpochNsFromTimestamp`/`TimestampNs`가 모두 `MonotonicNsFromTimestamp()`(정수 연산, `Frequency == 1e9`면 그대로) |
| Java | `bindings/java/perf/common/.../PerfMeasurement.java:125` | 이미 `System.nanoTime()` | 코드 변경 없음. 정책 근거 주석만 추가 |
| Node | `bindings/node/perf/common/perf_measurement.ts:22` | `BASE_EPOCH_NS(Date.now()) + hrtime delta` (**wall 기준점**) | `process.hrtime.bigint()` 그대로 |
| Node | `perf_measurement.ts:27` | — | `monotonicMs()` 신설(Number 기반 deadline 용) |
| Node | `perf/multi/perf_multi_runtime.ts:242-243` | `Date.now()` deadline | `monotonicMs()` |
| Node | `perf/multi/perf_multi_orchestrator.ts:861-863` | `Date.now()` deadline | `monotonicMs()` |
| Node | `perf/single/perf_single_common.ts:283-284, 307-308, 590, 599` | `Date.now()` deadline | `monotonicMs()` |

**perf 런타임 코드에 남은 `Date.now()`는 0건이다**(`bindings/node/perf/**/*.ts` 기준).

**부수 효과(개선)**: .NET·Node는 이전에 프로세스마다 `wall epoch ↔ monotonic` 앵커를 따로
잡았기 때문에 multi one-way latency(client stamp를 다른 프로세스인 server가 비교)에 앵커
차이만큼의 계통 오차가 있었다. 이제 두 프로세스가 같은 boot 축을 직접 읽는다.

C++는 같은 파일 안에서 deadline 판정은 `steady_clock`, latency는 `system_clock`으로
섞여 있었다(`perf_multi_reqrep.hpp:445-452`, `perf_single_reqrep.hpp` observe). 이제 일치한다.

---

## 2. 작업 F — 메시지당 환경 변수 조회 캐시

기준: C `bindings/c/perf/common/perf_zlink_part_helpers.hpp:13-24`
(`perf_measurement_part_count()`가 함수 스코프 `static const`, 주석에 "Read once per
process ... a per-call getenv only adds hot-path noise to every send/recv").
이 항목은 성능 최적화가 아니라 **하네스 계측 비용을 binding 쪽에만 부과하지 않기 위한
측정 조건 정합**이다(`PERF_POLICY.md:105-115`의 "같은 이름의 metric을 같은 anchor에서").

### 2.1 C++ — 찾은 메시지당 조회와 처리

| 파일:줄 | 변수 | 메시지당? | 처리 |
|---|---|---|---|
| `multi/common/perf_common.hpp:102` | `PERF_PART_COUNT` | **예** — `measurement_parts_valid()`가 2회 호출, 각 client/server의 send·recv 경로 6곳(`perf_pubsub_client.cpp:77`, `perf_dealer_router_client.cpp:183`, `perf_pubsub_server.cpp:93`, `perf_dealer_dealer_client.cpp:234`, `perf_router_router_client.cpp:229`, `perf_multi_reqrep.hpp:405,417,574`) | **캐시**(함수 스코프 `static const`) |
| `multi/common/perf_multi_reqrep.hpp:48` | `PERF_MULTI_REQREP_TIMEOUT_MS` | **예** — `launch_request()`가 요청 제출마다 호출(`:375`) | **캐시** |
| `multi/common/perf_multi_reqrep.hpp:61` | `PERF_MULTI_REQREP_MAX_OUTSTANDING` | 아니오(생성자 1회) | **캐시**(같은 함수군 일관성) |
| `multi/src/perf_dealer_dealer_client.cpp:43`, `multi/src/perf_router_router_client.cpp:30`, `multi/src/perf_stream_server.cpp:161`, `multi/common/perf_multi_routed_relay.hpp:23` | `PERF_DEBUG` | **예** — `debug_log()`/guard가 active recv·send 루프 안(`perf_router_router_client.cpp:343,348,369`) | **캐시**(`static const bool`) |
| `single/src/perf_pubsub.cpp:18`, `single/src/perf_dealer_router.cpp:13`, `single/src/perf_pair.cpp:15`, `single/src/perf_router_router.cpp:18` | `PERF_DEBUG` | **예** — recv 핸들러/루프 안 | **캐시** |
| `single/common/perf_single_common.hpp:42` | `PERF_PART_COUNT` | 예 | **이미 캐시됨**(변경 없음) |
| `single/common/perf_single_common.cpp:277` | `PERF_DEBUG` | 예 | **이미 캐시됨** |
| `single/common/perf_single_latency.hpp:105`, `common/perf_latency_sampler.hpp:100` | `PERF_*_LATENCY_SAMPLE_CAP` | 아니오(생성자 1회) | 그대로 |
| `multi/common/perf_common_multi.hpp:205-236`, `single/common/perf_single_common.cpp:142-188` | 각종 knob | 아니오(설정 파싱 1회) | 그대로 |
| `multi/common/perf_common.hpp:755-790` `debug_header_trace*` | `PERF_DEBUG_HEADER_TRACE(_LIMIT)` | 아니오 — **호출자가 없다(dead code)** | 그대로 |
| `multi/src/perf_stream_server.cpp:462` | `PERF_*` io timeout | 아니오(1회) | 그대로 |

### 2.2 .NET

| 파일:줄 | 변수 | 메시지당? | 처리 |
|---|---|---|---|
| `single/.../src/PerfReqRep.cs:220,223,376,379` → `ResolveReqRepTimeout()` | `PERF_SINGLE_REQREP_TIMEOUT_MS` | **예** — submit lambda가 요청마다 호출 | **캐시**(`static readonly TimeSpan ReqRepTimeout`, `PerfReqRep.cs:620-631`) |
| `multi/.../src/PerfMultiRoutedRelayServer.cs:217` `DebugFailure` | `PERF_DEBUG` | relay 실패 경로(반복 가능) | **캐시**(`static readonly bool DebugEnabled`) |
| `multi/.../src/PerfMultiStreamServer.cs:303` `DebugFailure` | `PERF_DEBUG` | packet 경로 | **캐시** |
| `common/.../PerfSocketIo.cs:8` `MeasurementPartCount` | `PERF_PART_COUNT` | 예 | **이미 static readonly**(변경 없음) |
| `single/.../common/PerfCommon.cs:8` `SinglePerfDebugEnabled` | `PERF_DEBUG` | 예 | **이미 static readonly** |
| `multi/.../src/PerfMultiSocketReqRep.cs:17` `s_debugEnabled` | `PERF_DEBUG` | 예 | **이미 static readonly** |
| `common/.../PerfEnv.cs`의 `Resolve*` 호출자 전수 | 각종 knob | 아니오 — 전부 `Run()` 진입부 또는 소켓 생성부 1회 | 그대로 |

### 2.3 Java — **메시지당 조회 0건, 변경 없음**

| 위치 | 상태 |
|---|---|
| `PerfUtil.java:33-34` `MEASUREMENT_PART_COUNT` | 이미 `static final` |
| `PerfMeasurement.java:19-20` `USE_NETTY_BYTEBUF_POOL` | 이미 `static final` |
| `PerfMetricsCollector.java:196-201` latency cap | 생성자 1회 |
| `PerfMultiSocketReqRep.java:276,405` | 각각 phase 진입부 1회(`:179,:181`) |
| `PerfMultiRoutedSendCoordinator.java:32` `sendDrainTimeout()` | 패턴 teardown 1회(`PerfMultiDealerDealer:188`, `PerfMultiDealerRouter:134`, `PerfMultiRouterRouter:139`) |
| `PerfArgs.java`, `PerfTransport.java`, `PerfStopToken.java` | 설정 파싱/종료 1회 |

### 2.4 Node

| 파일:줄 | 변수 | 처리 |
|---|---|---|
| `perf/multi/perf_multi_runtime.ts:30` | `PERF_PART_COUNT` | `measurementPartCount()`가 `measurementParts()`/`appendMeasurement()`에서 메시지마다 호출됨 → **모듈 상수** |
| `perf/single/perf_single_common.ts:43` | `PERF_PART_COUNT` | 〃 → **모듈 상수** |
| `perf/single/perf_single_common.ts:46` | `PERF_NODE_TRACE` | recv drain 루프 안 8곳(`:377,382,396,402,429,434,445,463`) → **모듈 상수** |
| `perf/multi/perf_multi_socket_reqrep.ts:31` | `PERF_PART_COUNT` | reply마다 `measurementPayload()` → **모듈 상수** |
| `perf/single/perf_socket_reqrep.ts:39` | `PERF_PART_COUNT` | request/reply마다 → **모듈 상수** |
| `perf/multi/perf_multi_dealer_dealer_server.ts:32` | `PERF_PART_COUNT` | recv 루프 안(`:113`) → **모듈 상수** |
| `perf/multi/perf_multi_routed_sendsend.ts:38` | `PERF_PART_COUNT` | echo recv 루프 안(`:355`) → **모듈 상수** |
| `perf/single/perf_single_sender_worker.ts:32,35,37` | `PERF_PART_COUNT`, `PERF_NODE_TRACE`, `PERF_NODE_MESSAGE_PAYLOAD` | `appendMeasurement()`·`submitOnce()`·replier recv가 메시지마다 → **모듈 상수 3개** |
| `perf/single/perf_pubsub.ts:35`, `perf/single/perf_router_router.ts:37` | `PERF_NODE_TRACE` | `trace()`가 메시지 경로에서 호출 → **모듈 상수** |
| `perf/common/perf_measurement.ts:300-301` | `PERF_*_LATENCY_SAMPLE_CAP` | collector 생성 1회 → 그대로 |
| `perf_multi_socket_reqrep.ts:77,88`, `perf_socket_reqrep.ts:139`, `perf_multi_routed_sendsend.ts:261` | timeout/상한 knob | 루프 밖으로 이미 hoist → 그대로 |
| `perf_multi_auto_hwm.ts:135-137`, `perf_multi_guards.ts`, `run_benchmarks.ts` | 설정/보고/orchestrator | 그대로 |

### 2.5 의미 변화가 없다는 확인

- 값·기본값·적용 범위·파싱 규칙을 **한 곳도 바꾸지 않았다.** 표현식을 그대로 옮겨
  1회 평가로만 만들었다.
- 러너가 **측정 도중 이 변수들을 바꾸는 곳은 없다.** 전수 검색 결과 perf 러너 안의 환경
  변수 쓰기는 두 곳뿐이다.
  - `bindings/cpp/perf/multi/common/perf_entry.hpp:19-20` — `setenv("PERF_PATTERN"/"PERF_MULTI_PATTERN")`,
    프로세스 진입 직후 1회. 두 변수 모두 이번에 캐시한 대상이 아니다.
  - `bindings/node/perf/multi/run_benchmarks.ts:159` — orchestrator가 **자식 프로세스에
    물려줄** `PERF_MULTI_MONITOR_HWM`을 설정. 벤치 프로세스는 spawn 시점에 이미 확정된
    환경을 받으며, 캐시 대상 변수도 아니다.
- 캐시 시점: C++는 함수 스코프 `static const`(첫 호출 시, thread-safe),
  .NET/Java는 타입 초기화 시, Node는 모듈 require 시. 모두 벤치 프로세스가 자기 환경을
  받은 뒤이므로 값이 달라질 수 없다.

---

## 3. 작업 B — multi `PERF_MULTI_REQREP_MAX_OUTSTANDING` (.NET·Node)

근거: `PERF_SINGLE_TEST_POLICY.md §1.1.3`("러너는 미완료 개수 상한 하나만 둔다"),
`PERF_MULTI_TEST_POLICY.md:164-168`(in-flight 1 금지 = 상한을 왕복 gate로 쓰면 안 됨),
직전 pass §2.3(값 64, 하한 2 clamp, 7개 binding 공통).

| 언어 | 파일:줄 | 변경 |
|---|---|---|
| .NET | `PerfMultiSocketReqRep.cs:551-566` | `ResolveReqRepMaxOutstanding()`(`PERF_MULTI_REQREP_MAX_OUTSTANDING`, 기본 64, `Math.Max(2, ...)`) |
| .NET | 〃 `:579` | `ClientSlot.Outstanding` 카운터(Interlocked/Volatile) |
| .NET | 〃 `:235` | 루프 진입 전 `maxOutstanding` 1회 해석 |
| .NET | 〃 `:355-362` | submit 루프의 socket별 gate + 성공 시 증가 |
| .NET | 〃 `:333` | `ObserveRequestAsync` finally에서 감소 |
| Node | `perf_multi_socket_reqrep.ts:78-84` | `maxOutstanding` 해석(같은 이름·기본값·clamp) |
| Node | 〃 `:86` | `outstandingPerSocket[]` |
| Node | 〃 `:113` | submit 루프의 socket별 gate |
| Node | 〃 `:103,:123` | 증감 |

C++·Java는 직전 pass에서 완료. **4개 언어의 knob 이름·기본값·clamp가 동일하다.**
Rust·Python은 D-BP8 후순위로 여전히 미적용(§8-4).

---

## 4. 작업 C — .NET SENDSEND(routed one-way) server의 stdin STOP

근거: `PERF_POLICY.md:138-143`("perf runner와 benchmark process 사이의 handshake 방식은
`bindings/c/perf` 구현을 기준 계약으로 고정한다 … 같은 stdout/stdin token, 같은 전송
방향, 같은 start/stop 의미"), C 기준
`bindings/c/perf/multi/common/perf_multi_relay_server.hpp:664-677`(stdin watcher) +
`bindings/c/perf/run_comparison.py:1759-1775`(모든 multi server에 stdin `STOP`).

확인한 사실:
- C의 `DEALER_ROUTER`/`ROUTER_ROUTER`(및 `_SENDSEND`) server = relay server이고,
  **stdin `STOP`/`QUIT`로만** 종료한다. wire stop token은 쓰지 않는다.
- .NET relay server는 wire stop token만 감지했고(`PerfMultiRoutedRelayServer.cs:51`),
  그 토큰을 **보내는 client가 없다**(`PerfClientHelpers.SendStopTokenNoWait`은 호출자 0).
  runner의 `pattern_uses_control_pipe`에도 이 4개 패턴이 없어 server에 stdin pipe 자체가
  없었고, 마지막에 `terminate_running_pid_or_fail_if_exited`(SIGTERM)로 죽었다.

| 파일:줄 | 변경 |
|---|---|
| `PerfMultiRoutedRelayServer.cs:26-41` | stdin watcher thread 추가(`STOP`/`QUIT` 또는 EOF → `stopRequested`), C relay server 준용 |
| 〃 `:52` | relay 루프 조건에 `stopRequested` 추가. poll은 이미 bounded(`pollTimeoutMs`, 기본 200 ms)여서 다음 턴에 관측된다 |
| `perf/multi/run_benchmarks.sh:575-590` | `pattern_uses_control_pipe`에 `MULTI_DEALER_ROUTER`, `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER`, `MULTI_ROUTER_ROUTER_SENDSEND` 추가 |
| 〃 `:2137-2196` | 해당 branch: client 종료 뒤 server에 `STOP` 전송 → `wait_for_pid_exit_zero`로 **정상 종료(exit 0) 확인** → fd close. 실패 경로에도 `STOP`+fd close 추가(fd 누수 방지) |

기존 wire stop token 감지는 남겼다(무해하며 최소 변경).

**회귀 위험(감독자 확인 필요)**: 이전에는 SIGTERM 종료라 exit code를 보지 않았으나
이제 exit 0을 요구한다. relay server가 STOP에 깨끗이 끝나지 않으면 `process_exit_nonzero`
실패가 된다 — smoke 목록 §6.1 #5~#8이 이것을 본다.

---

## 5. 작업 D — single 러너 정책 정합

### 5.1 pattern 등록 (D-BP6, `PERF_SINGLE_TEST_POLICY.md:29-31`)

4개 언어 모두 **이미 7개**다. 변경 없음.

| 언어 | 위치 |
|---|---|
| C++ | `perf/run_binding_single.sh:97` `STANDARD_PATTERNS`, `single/run_comparison.py:28-46` |
| .NET | `single/.../common/PerfPatternRegistry.cs:8-21` |
| Java | `single/.../single/PerfPatternRegistry.java:14-20` |
| Node | `perf/single/run_benchmarks.ts:31-37` |

### 5.2 single REQREP 실행 모델 — before / after

정책: `PERF_SINGLE_TEST_POLICY.md`
§1.1.0("동기 실행 모델은 '한 건 보내고 응답을 기다린다'는 뜻이 아니다 … RTT 전용 루프는 금지"),
§1.1.1(admission ≠ reply), §1.1.2(의사코드: 포화까지 연속 제출 → completion drain 교대),
§1.1.3(awaitable terminal이면 기다리지 말고 계속 제출, **미완료 개수 상한 하나만**,
executor/event loop가 아니라 전용 thread가 completion을 진행),
§1.1.4(recv 모델, 언어별 고정 조건).
기준 구현: C `bindings/c/perf/single/common/perf_single_reqrep.hpp:399-465`(커밋 `074d2a5964`).

| 언어 | before | after |
|---|---|---|
| C(기준) | `submit_request()`를 backpressure까지 연속 호출 → 64건마다 + 라운드마다 `poll_completion_once()` → deadline 후 bounded drain | (수정 대상 아님) |
| **C++** | requester thread가 **blocking `.submit()`** 터미널을 호출해 reply까지 대기 → **in-flight 1**. 별도 completion thread가 poller를 돌렸지만 진행시킬 것이 없었다 | awaitable `.async()`를 **await 없이** 연속 제출. 미완료 상한(`PERF_SINGLE_REQREP_MAX_OUTSTANDING`, 기본 64, 하한 2)만 둔다. **calling thread(= 전용 requester OS thread)**가 `POLLCOMPLETION` poller를 소유하고 자기 ready queue를 `run_ready_round()`로 돌려 completion을 직접 진행. 별도 completion thread 삭제 |
| **.NET** | 이미 전용 requester thread가 `Async()`를 await 없이 연속 제출하고, 별도 progress thread가 `POLLCOMPLETION` poller를 소유 — **상한만 없었다** | 상한 추가. 나머지 구조 유지(최소 변경) |
| **Java** | main thread가 `submit_sync()`(blocking) → reply 받고 다음 제출 → **in-flight 1** | `submit()`(CompletionStage)을 await 없이 상한까지 연속 제출. **같은 thread**가 `PerfSocketPollSet(POLLCOMPLETION)`을 소유하고 `poll(0)`/`poll(≤50 ms)`로 completion을 직접 dispatch(§1.1.2 교대 루프). deadline 후 bounded completion drain |
| **Node** | main thread가 `submit_sync()`(blocking) → **in-flight 1** | `submit()`(Promise)을 await 없이 상한까지 연속 제출, 미완료 Promise 집합에서 완료분부터 drain, 라운드마다 `sleepImmediate()`(`setImmediate`) 한 턴. deadline 후 남은 것만 bounded drain |

변경 파일:줄 —
C++ `single/common/perf_single_reqrep.hpp:31`(상한 knob), `:44-51`(launch 상태),
`:88-102`(ready queue bind awaiter), `:217-253`(`begin_reqrep_request` = `.async()` 터미널),
`:257-325`(`submit_async_request` detached coroutine), `:329-355`(`close_requester_and_drain`
— bounded drain 실패 시 ready queue를 suspended coroutine 아래에서 파괴하지 않는 teardown),
`:449-556`(requester 루프: 연속 제출 ↔ `progress_once()` 교대, 64건마다 progress, deadline 후 drain).
Java `single/.../PerfSocketReqRep.java:116-118`, `:147-220`(`runRequestPhase`),
`:222-285`(`submitRequest`), `:288-292`(`resolveMaxOutstanding`), `:294-325`(보조 판정).
.NET `single/.../PerfReqRep.cs:641-655`(`ResolveReqRepMaxOutstanding`), `:471`, `:476-486`(gate).
Node `perf/single/perf_socket_reqrep.ts:124-174`.

**상한 knob**: `PERF_SINGLE_REQREP_MAX_OUTSTANDING`, 기본 **64**, 하한 2 clamp,
**4개 언어 동일**. multi의 `PERF_MULTI_REQREP_MAX_OUTSTANDING`과 같은 값·같은 의미이고
suite 접두사만 다르다(`PERF_SINGLE_REQREP_TIMEOUT_MS` / `PERF_MULTI_REQREP_TIMEOUT_MS`와
같은 관례).

### 5.3 §1.1.4의 문언과 §1.1.3의 관계 — **감독자 확인 필요(§8-2)**

§1.1.4는 "C++은 `co_await`, .NET은 `Task`, Rust는 Future executor를 single 측정 경로에서
사용하지 않는다"고 적혀 있다. 그런데 §1.1.3은 admission과 reply를 하나의 awaitable로
합쳐 제공하는 공개 터미널에 대해 "그 awaitable을 기다리지 말고 계속 제출 … 전용 requester
thread가 직접 completion을 진행시켜 완료시킨다"를 요구한다.

공개 API 사실:
- C++ `request_submit_operation_t`의 터미널은 **`async()`(awaitable)와 `submit()`(blocking)
  둘뿐**이며(`bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:305-330`),
  `async_result_t<T>`는 **`operator co_await() &&`만** 노출한다(`:139-140`). 코루틴 없이
  결과를 취하는 공개 경로가 없다.
- .NET은 `Async()` → `Task<...>`뿐(`Contracts/Messaging/OperationContracts.cs:165-178`).
- Node는 `submit()` → `Promise`, `submit_sync()` → blocking(`contracts/messaging/operations.ts:48-56`).
- Java만 `submit()`(CompletionStage) + 공개 poller가 completion dispatch를 소유하므로
  코루틴/Task/Promise 없이 §1.1.2를 그대로 구현할 수 있다.

이번 pass는 §1.1.4의 금지를 "**진행(progress)을 언어 런타임의 executor·event loop에
맡기지 말라**"로 읽고 §1.1.3을 적용했다(감독자 지시문의 명시적 지침과 일치).
그래서:
- C++는 코루틴을 쓰되 **continuation scheduler를 러너 자신의 `application_ready_queue_t`로
  묶어** 전용 requester thread가 `run_ready_round()`로 직접 재개한다. Core의 async runtime,
  thread pool, timer는 쓰지 않는다(multi 러너가 이미 쓰는 검증된 구조 —
  `bindings/cpp/perf/common/perf_socket_adapter.hpp:44-197`, `:570-596`).
- .NET은 `Task`를 만들지만 `await` 이후 continuation이 **poller를 소유한 전용 progress
  thread**에서 실행된다(`Runtime/Messaging/CompletionOwner.cs:592-645` — 공개 drain owner가
  있으면 런타임 pump를 띄우지 않는다).
- Node는 **worker/main thread 자신의 microtask 턴**이 필요하다. `sleepImmediate()`가 그
  턴이며, 이것이 §1.1.0의 "event-loop yield" 금지 문언과 **직접 충돌한다**. Node에는
  이 충돌을 피하면서 in-flight > 1을 만드는 공개 경로가 없다(대안은 §1.1.2가 허용하는
  "전용 requester thread를 여러 개" = worker 여러 개뿐이며, 이는 측정 조건 변경이다).
  → **감독자 판단 항목**(§8-2).

### 5.4 single 러너의 그 밖의 §1.1.4 조건

- recv 모델(poller POLLIN + nonblocking recv drain): 4개 언어 one-way 5패턴 모두 유지, 변경 없음.
- Node는 replier가 `worker_threads`(`perf_single_sender_worker.ts`) — 유지.
- C++ single 측정 경로의 `co_await`는 위 §5.3의 REQREP 2패턴에만 새로 생겼다.
  one-way 5패턴에는 없다.

---

## 6. 작업 E — 측정 anchor 대조표

`PERF_POLICY.md:105-115`의 anchor(send timestamp 기록, ready 만족 판정, active 시작/종료
판정, 유효 recv 판정, throughput count 증가, latency sample 채취, RESULT line 확정).

### 6.1 single request-reply (`DEALER_ROUTER_REQREP` / `ROUTER_ROUTER_REQREP`) — 이번 pass 대상

| anchor | C(기준) | C++ | .NET | Java | Node |
|---|---|---|---|---|---|
| send ts 기록 | `perf_single_reqrep.hpp:235-239` submit 직전 `now_ns()` stamp(재시도 시 재stamp 안 함) | `perf_single_reqrep.hpp:487-492` submit 직전 stamp | `PerfReqRep.cs:485-488` `Async()` 직전 `sentTicks` | `PerfSocketReqRep.java:236-238` submit 직전 `PerfUtil.payload(..., nanoTime())` | `perf_socket_reqrep.ts:156-159` submit 직전 `stampPayload` |
| ready 만족 | monitor `CONNECTION_READY` + (RR) PING/PONG handshake | 동일(`:120-164` handshake) | 동일 | 동일(`:80-98`) | monitor ready + routing probe(`:106-108`) |
| active 시작/종료 | `run_request_phase` 진입 시 `deadline = now + duration`(`:334-336`) | `:464-471` 동일 | `DeadlineTicksFromSeconds`(`:473`) | `activeEnd = nanoTime() + duration`(`:107`) | `activeStartNs`/`activeStopNs`(`:112-114`) |
| 유효 recv 판정 | `record_request_completion`: header decode + `is_expected(run_id, phase_active, size)` + `completed_at < active_deadline`(`:165-192`) | `observe_request_completion`(`:114-146`) 동일 | `:518-528` decode + `completionTicks < deadlineTicks` | `runRequestPhase` completion: `receivedAt < activeEnd` + `decodeHeader` + `phase == ACTIVE`(`:155-176`) | `collector.recordPayload(..., currentEpochNs())` → `isWithinMeasurementBounds`(recv ∈ [start, stop], recv ≥ sent) |
| throughput count 증가 | 같은 블록 안 `completed.fetch_add(1)` | 동일 | `Interlocked.Increment(ref completed)` (같은 블록) | `metrics.recordNanos()`가 count+sum 동시 증가 | collector `accepted += 1` |
| latency sample 채취 | 같은 블록 `latency.add(now_ns − sent_ts_ns)`(왕복 전체, ÷2 없음) | 동일 | `ReservoirSample(nowNs − SentTsNs)` | `recordNanos(receivedAt − sent_ts)` | `addLatency(recv − sent)`, `roundTrip:false` |
| RESULT 확정 | 루프·drain 종료 후 `print_reqrep_result`, `throughput = completed / duration_s`, `bandwidth = tp × size × 2 / 1e6` | `emit_reqrep_result`(`:560-563`) 동일 | `PrintResult`(단일 진입) 동일 | `metrics.finishSingle()` + `isEcho` ×2 | `summarizeMetrics(..., accepted, meanNs)` + `isEchoPattern` ×2 |

**어긋난 곳과 조치**
1. C++ — `submit()` blocking 터미널 때문에 count/latency가 사실상 in-flight 1의 관측이었다.
   §5.2에서 교정.
2. Java — 동일. §5.2에서 교정.
3. Node — 동일. §5.2에서 교정.
4. .NET — anchor 자체는 C와 일치. `completed` 증가가 `nowNs >= SentTsNs` guard 밖에 있어
   C(`:190-192`, guard 안)와 미세하게 다르지만, **시간원이 monotonic이 된 뒤에는 이 조건이
   거짓이 될 수 없으므로** 관측 가능한 차이가 없다. 최소 변경 원칙에 따라 두었다(§8-3).

### 6.2 multi request-reply — 이번 pass 대상(작업 B)

상한 도입은 anchor를 옮기지 않는다. 유효 recv/카운트/샘플 anchor는
`.NET PerfMultiSocketReqRep.cs:296-315`, `Node perf_multi_socket_reqrep.ts:95-98`로
직전 pass에서 확정된 위치 그대로다(둘 다 `completionTicks < deadlineTicks` /
collector bounds, latency ×0.5 = one-way 반환).

### 6.3 one-way(single 5패턴 / multi SENDSEND·PUBSUB·DEALER_DEALER)

이번 pass에서 anchor를 옮긴 곳이 없다. 시간원만 바뀌었고(§1),
`sent_ts_ns`와 수신 판정 시각이 **같은 시간원**을 쓰게 되어 anchor 의미가 오히려 강화됐다.
직전 조건 정합 pass(`2026-09-07-runner-conditions-pass1.ko.md`)와 설계 문서 §3.3의
one-way anchor 항목(R4~R8: deadline 필터 부재, wire 길이 검증 부재)은 **이번 범위 밖이며
여전히 미처리**다(§8-5).

---

## 7. 검증 (perf 미실행)

| 대상 | 결과 |
|---|---|
| C++ single 7 target 빌드(`cpp_perf_*`) | 통과 |
| C++ multi 13 target 빌드(`cpp_comp_src_*`) | 통과(warning 0) |
| .NET `Zlink.BindingBench`(single) Release 빌드 | 통과 (0 warning / 0 error) |
| .NET `Zlink.BindingBench.Multi` Release 빌드 | 통과 (0 warning / 0 error) |
| Java `:perf-single:compileJava`, `:perf-multi:compileJava` (offline) | 통과 |
| Node `tsc -p tsconfig.tools.json` + `dist-tools` 재생성 | 통과 |
| Node perf contract test 6종 | 통과 (2/3/4/10/5/1 = 25건) |
| `bash -n bindings/dotnet/perf/multi/run_benchmarks.sh` | 통과 |
| 시간원 cross-process 실측(C/C++/Java/Node/.NET) | §1.1 표 |
| Java `:perf-*:test` | **미실행** — `ZLINK_CORE_PACKAGE_PREFIX` 필요 |
| Python `pytest`/`unittest` | 해당 없음(Python 범위 밖) |

---

## 8. 감독자가 실행해야 할 smoke 목록

모두 **직렬**, `PERF_FAIL_FAST=1`, `--duration 1 --runs 1 --msg-sizes 64 --transports tcp`,
`load_avg < 10`. 측정 조건(duration/HWM/timeout/client 수)은 하나도 완화하지 않았다.

### 8.1 필수 — 이번 변경이 직접 건드린 경로

| # | 러너 | suite/pattern | 확인 항목 |
|---|---|---|---|
| 1 | C++ | single `DEALER_ROUTER_REQREP` | **실행 모델 교체.** `status: complete`, throughput이 in-flight 1 수준(수천 ops/s)이 아니라 C(774,901 / 849,108 ops/s) 자릿수에 근접, drain timeout 로그 없음, 프로세스가 hang 없이 exit 0 |
| 2 | C++ | single `ROUTER_ROUTER_REQREP` | 〃 |
| 3 | Java | single `DEALER_ROUTER_REQREP` | 〃 |
| 4 | Java | single `ROUTER_ROUTER_REQREP` | 〃 |
| 5 | Node | single `DEALER_ROUTER_REQREP` | 〃 (§8-2 결정 전이면 값만 기록) |
| 6 | Node | single `ROUTER_ROUTER_REQREP` | 〃 |
| 7 | .NET | single `DEALER_ROUTER_REQREP` | 상한 도입 회귀 없음(before 대비 throughput 변화 없어야 정상) |
| 8 | .NET | multi `MULTI_DEALER_ROUTER_SENDSEND` | **작업 C.** server가 stdin `STOP`으로 **exit 0**, `process_exit_nonzero` 없음, `status: complete` |
| 9 | .NET | multi `MULTI_ROUTER_ROUTER_SENDSEND` | 〃 |
| 10 | .NET | multi `MULTI_DEALER_ROUTER` | 〃 (matched-client 경로도 같은 relay server) |
| 11 | .NET | multi `MULTI_ROUTER_ROUTER` | 〃 |
| 12 | .NET | multi `MULTI_DEALER_ROUTER_REQREP` | 상한 도입 회귀 없음 |
| 13 | Node | multi `MULTI_DEALER_ROUTER_REQREP` | 〃 |
| 14 | C++ | single 5개 one-way(`PAIR`,`PUBSUB`,`DEALER_DEALER`,`DEALER_ROUTER`,`ROUTER_ROUTER`) | **시간원 교체 + getenv 캐시.** latency가 음수/0으로 무너지지 않는지, throughput이 캐시 효과로 상승(C의 −9.4% Ir/msg에 대응) |
| 15 | C++ | multi `MULTI_DEALER_DEALER`, `MULTI_PUBSUB` | 〃 (client stamp를 다른 프로세스 server가 비교하는 경로) |
| 16 | .NET·Java·Node | multi `MULTI_DEALER_DEALER` 각 1회 | 시간원 교체 뒤 one-way latency가 정상 범위인지 |
| 17 | Node | single `PAIR`, `PUBSUB` | `PERF_PART_COUNT`/`PERF_NODE_TRACE` 캐시 뒤 회귀 없음 |

### 8.2 상한이 측정을 제한하지 않는지

single·multi REQREP을 `PERF_SINGLE_REQREP_MAX_OUTSTANDING=256`
/ `PERF_MULTI_REQREP_MAX_OUTSTANDING=256`으로 한 번 더 돌린다.
RESULT가 기본값(64)과 유의미하게 다르면 상한이 측정을 제한하고 있다는 뜻이므로
값을 올리고 근거를 갱신해야 한다.

### 8.3 회귀 감시

- single REQREP 4개 언어는 **실행 모델이 바뀌었으므로 수치가 크게 달라진다.**
  `PERF_SINGLE_REQREP_TIMEOUT_MS` 초과가 0인지, 메모리가 duration에 비례해 증가하지 않는지.
- C++ single REQREP은 실패 경로에서 `close_requester_and_drain()`이 도는데, 이 함수는
  Core가 close를 받아들이지 않으면 `std::terminate()`한다(multi 러너와 같은 fail-closed
  정책). 실패 smoke에서 abort가 나오면 그 자체가 보고 대상이다.

---

## 9. 남은 항목 / 감독자 판단이 필요한 사항

1. **Java `System.nanoTime()`의 규격 보장.** javadoc은 원점이 임의라고만 하고
   프로세스 간 비교를 규격으로 보장하지 않는다. 이 호스트(HotSpot/Linux)에서
   `CLOCK_MONOTONIC`과 같은 축임을 실측했다(§1.1). Windows 측정으로 넘어갈 때는
   같은 실측을 다시 해야 한다(개정 문서 §6-3의 미확인 항목과 같은 성격).
2. **Node single REQREP의 event-loop 턴(§5.3).** §1.1.3을 적용하면 Node에서는 미완료
   Promise를 진행시키기 위해 해당 thread의 microtask 턴이 필요하고, 이는 §1.1.0의
   "event-loop yield 금지"와 문언상 충돌한다. 선택지:
   (a) 현재 구현 유지 + 정책 §1.1.4에 "Node의 awaitable 터미널은 그 전용 thread 자신의
   loop 턴으로 진행시킨다"를 명시, (b) worker를 N개 띄워 `submit_sync()` 유지(§1.1.2가
   허용하지만 N이 측정 조건이 된다), (c) Node single REQREP을 `UNSUPPORTED`로 남긴다.
   **결정 전까지 (a) 형태로 구현해 두었다.**
   같은 논지가 C++의 `co_await`(§1.1.4 문언)에도 적용된다.
3. **.NET single REQREP의 `completed` 증가 위치.** C는 `now >= sent` guard 안,
   .NET은 밖(§6.1-4). monotonic 시간원에서는 관측 차이가 없어 최소 변경으로 두었다.
   C와 1:1 일치를 요구하면 한 줄 이동으로 끝난다.
4. **Rust·Python(D-BP8 후순위).** multi REQREP 상한 미적용, Go REQREP 재구성,
   Go send drain, Go/Rust/Python `AUTO_HWM_DETAIL`, Python 시간원, Rust·Python single
   REQREP 실행 모델, Rust·Python getenv 캐시 — 전부 **미착수**이며 위반 상태 기록은 유지된다.
5. **설계 문서 §3.3 / 개정 R4~R18의 one-way 잔여 항목**(active deadline 필터 부재,
   wire 길이 검증 부재, transient 재시도 busy-spin, RESULT 정밀도, latency 표본 0개 fallback,
   Node `inproc` UNSUPPORTED) — 이번 pass 범위 밖. 별도 pass 필요.
6. **`reqrep_max_outstanding`의 Effective Options 노출**(직전 pass §8-1)은 여전히 미결이며
   single 상한도 같은 결정에 묶인다. `PERF_SINGLE_TEST_POLICY.md §1.1.3`은 "report의
   Effective Options에 노출한다"고 요구하지만, C 러너에 대응 행이 없어 노출하면 4개 언어가
   C와 1행 달라진다. **결정 전까지 노출하지 않았다.**
7. **Java `:perf-single:test` / `:perf-multi:test` 미실행** — `ZLINK_CORE_PACKAGE_PREFIX`
   준비 뒤 감독자가 돌려야 한다.

---

## 10. 변경 파일

```
bindings/cpp/perf/multi/common/perf_common.hpp
bindings/cpp/perf/multi/common/perf_metric_header.hpp
bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp
bindings/cpp/perf/multi/common/perf_multi_routed_relay.hpp
bindings/cpp/perf/multi/src/perf_dealer_dealer_client.cpp
bindings/cpp/perf/multi/src/perf_router_router_client.cpp
bindings/cpp/perf/multi/src/perf_stream_server.cpp
bindings/cpp/perf/single/common/perf_single_metric_header.hpp
bindings/cpp/perf/single/common/perf_single_reqrep.hpp
bindings/cpp/perf/single/src/perf_dealer_router.cpp
bindings/cpp/perf/single/src/perf_pair.cpp
bindings/cpp/perf/single/src/perf_pubsub.cpp
bindings/cpp/perf/single/src/perf_router_router.cpp
bindings/dotnet/perf/common/Zlink.BindingBench.Common/PerfShared.cs
bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiRoutedRelayServer.cs
bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiSocketReqRep.cs
bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiStreamServer.cs
bindings/dotnet/perf/multi/run_benchmarks.sh
bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs
bindings/java/perf/common/src/main/java/systems/zlink/perf/PerfMeasurement.java
bindings/java/perf/single/Zlink.BindingBench/src/main/java/systems/zlink/perf/single/PerfSocketReqRep.java
bindings/node/perf/common/perf_measurement.ts
bindings/node/perf/multi/perf_multi_dealer_dealer_server.ts
bindings/node/perf/multi/perf_multi_orchestrator.ts
bindings/node/perf/multi/perf_multi_routed_sendsend.ts
bindings/node/perf/multi/perf_multi_runtime.ts
bindings/node/perf/multi/perf_multi_socket_reqrep.ts
bindings/node/perf/single/perf_pubsub.ts
bindings/node/perf/single/perf_router_router.ts
bindings/node/perf/single/perf_single_common.ts
bindings/node/perf/single/perf_single_sender_worker.ts
bindings/node/perf/single/perf_socket_reqrep.ts
bindings/node/dist-tools/perf/**                 (위 TS의 추적 산출물, tsc 재생성)
doc/perf/perf/bindings-0.17.0/log/2026-09-07-priority-runner-parity.ko.md   (본 문서)
```

Go·Rust·Python 러너와 C 러너(`bindings/c/perf/**`), binding 라이브러리 소스는 수정하지 않았다.
