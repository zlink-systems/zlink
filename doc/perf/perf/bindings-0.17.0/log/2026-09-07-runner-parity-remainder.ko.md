# 러너 정합 잔여 항목 완료 기록

- 날짜: 2026-09-07
- 범위: `bindings/{c,cpp,dotnet,java,rust,go,node,python}/perf`
- Core: `release`, `/home/hep7/.cache/zlink/core-pinned/0.17.1`
- 판정 성격: 정책 정합. paired 성능 측정과 처리량 개선 판정은 하지 않았다.

## 1. 판정 근거

- `PERF_POLICY.md:109-123`: percentile 선형 보간식과 표본 0개/cap 0의
  `p95 == p99 == mean` 규칙.
- `PERF_POLICY.md:128-140`: 측정 시간원은 프로세스 간 같은 기준점을 공유하는
  monotonic clock 하나만 사용한다.
- `PERF_POLICY.md:147-157`: C와 binding은 ready, active, 유효 recv, throughput
  count, latency sample, RESULT 확정 anchor를 같은 의미로 둔다.
- `PERF_POLICY.md:1087-1112`: Effective Options를 start/result 양쪽에 남기고,
  throughput/bandwidth는 3자리, latency 계열은 6자리 고정 소수점으로 출력한다.
- `PERF_SINGLE_TEST_POLICY.md:239-245`: one-way transient
  `EAGAIN/EINTR/ETIMEDOUT`은 정확히 1 ms 대기하고, 같은 sequence를 새
  `sent_ts_ns`로 다시 stamp해 제출한다.
- `PERF_SINGLE_TEST_POLICY.md:375-403`: 실제 wire 길이까지 맞는 active header와
  active deadline 이전 수신만 집계하며, deadline 판정과 latency에는 같은 recv
  monotonic timestamp를 사용한다.
- `PERF_MULTI_TEST_POLICY.md:642-646,663-720`: multi도 active 구간의 유효 recv
  집합 하나에서 count와 latency를 산출하고 C와 같은 header/phase 경계를 사용한다.
- `decisions.ko.md:1904-1910` D-BP13: 8개 러너 옵션 노출, .NET completion
  count anchor, one-way 잔여 5건 이식의 감독자 판정.

## 2. 완료 결과

### 2.1 `reqrep_max_outstanding`

8개 러너의 single/multi `Effective Options (start)`와 `(result)`에
`reqrep_max_outstanding`을 항상 출력한다.

| 러너 | single | multi | 의미 |
|---|---:|---:|---|
| C | `backpressure` | `backpressure` | app 상한 없이 Core admission backpressure가 경계 |
| C++ | 실제 적용값(기본 64, 최소 2) | 실제 적용값(기본 64, 최소 2) | `PERF_*_REQREP_MAX_OUTSTANDING` |
| .NET | 실제 적용값(기본 64, 최소 2) | 실제 적용값(기본 64, 최소 2) | 동일 |
| Java | 실제 적용값(기본 64, 최소 2) | 실제 적용값(기본 64, 1 이하는 기본값) | 기존 runtime resolver와 동일 |
| Rust | 실제 적용값(기본 64, 최소 2) | 실제 적용값(기본 64, 최소 2) | 동일 |
| Go | 실제 적용값(기본 64, 최소 2) | 실제 적용값(기본 64, 최소 2) | 동일 |
| Node | 실제 적용값(기본 64, 최소 2) | 실제 적용값(기본 64, 최소 2) | 동일 |
| Python | 실제 적용값(기본 64, 최소 2) | 실제 적용값(기본 64, 최소 2) | 동일 |

status smoke의 16개 start/result 섹션에서 C는 `backpressure`, 나머지는 기본
실제 적용값 `64`로 확인했다.

### 2.2 .NET `completed` anchor

직전 기록의 지적과 달리 현재 코드는 이미 C와 같은 순서였다. C는
`bindings/c/perf/single/common/perf_single_reqrep.hpp:191-200`에서 completion
deadline과 `now >= sent_ts_ns`를 모두 통과한 뒤 `completed`를 증가시킨다. .NET도
`bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs:467-475`에서 같은
guard 안에서 `completed++` 한다. 따라서 의미 없는 재편집은 하지 않았고, source
대조로 B 항목을 no-op 완료 처리했다.

### 2.3 one-way 잔여 5건

- active deadline: single 5개 one-way 패턴 모두 recv 직후 monotonic timestamp를
  한 번만 읽어 `< active_deadline` 판정과 latency 산출에 재사용한다.
- wire 길이: header 최소 길이가 아니라 `max(configured_size, header_size)`와 실제
  payload byte 길이가 정확히 같을 때만 집계한다. 불일치는 비 fatal 제외다.
- transient: `EAGAIN/EINTR/ETIMEDOUT`과 binding의 동등 backpressure 결과에서
  정확히 1 ms 대기한다. sequence는 성공할 때만 전진하고 매 시도 timestamp는 새로
  stamp한다.
- RESULT: throughput/bandwidth 3자리, latency/latency_p95/latency_p99 6자리다.
  anchor 전수 대조에서 공통 multi reporter를 거치지 않는 C `MULTI_STREAM` raw
  client의 latency 3자리 잔여도 발견해 6자리로 맞췄다.
- 표본 0개: exact count/sum은 유지하고 reservoir가 비었으면 p95/p99를 exact mean으로
  보고한다. .NET single은 cap 0에서 오직 running mean 하나만 유지해 같은 결과를 낸다.

언어별 예외 규칙 수는 수정 전 8종(옵션 누락/조건부 노출, deadline 이중 시각,
길이 하한 비교, transient 부분 집합, busy/암묵 재시도, sequence 선증가, 기본 포맷,
빈 reservoir 0 fallback)에서 수정 후 0종이다. 결과적으로 C 정책의 규칙 하나씩만
각 책임 지점에 남겼다.

## 3. 측정 anchor 6종 대조

아래 표는 각 suite의 대표 one-way 경로(PAIR, MULTI_PUBSUB)와 공통 collector/reporter
owner를 적었다. 같은 suite의 나머지 pattern entry point도 동일 순서로 전수 대조했다.
`R/A/V/C/L/O`는 각각 ready / active / valid recv / count / latency / output이다.

### 3.1 single

| 러너 | R: ready 만족 | A: active 시작/종료 | V: 유효 recv | C: count 증가 | L: latency 채취 | O: RESULT 확정 |
|---|---|---|---|---|---|---|
| C | monitor 성공 뒤 (`perf_single_monitor.hpp:120-156`) | deadline 생성/송신 종료 (`perf_single_one_way.hpp:302-352`) | recv 직후 동일 시각+header (`:323-333`) | guard 안 (`:334`) | 같은 recv 시각 (`:335`) | 공통 printer (`perf_single_monitor.hpp:172-183`) |
| C++ | 연결 gate 뒤 (`perf_pair.cpp:108-112`) | deadline 생성/송신 종료 (`:131-191`) | exact length/header/deadline (`:47-67`) | guard 안 (`:71`) | 같은 recv 시각 (`:72`) | 공통 printer (`perf_single_report.hpp:32-44`) |
| .NET | monitor gate 뒤 (`PerfPair.cs:38-39`) | deadline 생성/송신 종료 (`:83-107`) | exact header+recv deadline (`:158-165`) | guard 안 (`:167`) | 같은 `recvTicks` (`:168-173`) | 공통 printer (`PerfShared.cs:171-179`) |
| Java | 두 monitor gate 뒤 (`PerfPair.java:51-54`) | `nanoTime` deadline (`:59-60,117`) | exact header+같은 recv 시각 (`:78-90`) | `recordNanos` 안 (`PerfMetricsCollector.java:41-46`) | 같은 `receivedNanoTime` (`PerfPair.java:82-91`) | 공통 formatter (`PerfReport.java:26-44`) |
| Rust | 두 monitor gate 뒤 (`perf_pair.rs:43-45`) | monotonic ns deadline (`:52-57`) | 공통 validator+deadline (`common.rs:648-656`) | `record_ns` 안 (`:446-448`) | 같은 `recv_ts_ns` (`:654-658`) | 공통 printer (`:601-611`) |
| Go | monitor gate 뒤 (`perf_pair.go:34`) | 공통 window (`perf_oneway.go:43-69`) | exact header+window (`runtime.go:45-50`) | guard 안 (`:52`) | 같은 `nowNs` (`:53-54`) | 공통 printer (`common.go:161-166`) |
| Node | monitor gate 뒤 (`perf_pair.ts:44-63`) | monotonic start/stop (`:65-73`) | collector exact header+window (`perf_measurement.ts:344-365`) | guard 안 (`:366`) | 같은 `recvTsNs` (`:367`) | 공통 formatter (`:215-228`) |
| Python | monitor gate 뒤 (`perf_pair.py:75-83`) | `perf_counter` deadline (`:85-98`) | exact header+recv deadline (`perf_common.py:346-360`) | guard 안 (`:361`) | 같은 `recv_ts_ns` (`:362-363`) | 공통 printer (`perf_metrics.py:273-277`) |

### 3.2 multi

| 러너 | R: ready 만족 | A: active 시작/종료 | V: 유효 recv | C: count 증가 | L: latency 채취 | O: RESULT 확정 |
|---|---|---|---|---|---|---|
| C | client monitor gate (`perf_multi_pubsub_client.cpp:326-339`) | steady deadline (`:162-173,202`) | recv/header/phase (`:210-232`) | guard 안 (`:235`) | 같은 유효 집합 (`:236-239`) | 공통 printer (`perf_multi_metrics.hpp:225-236`) |
| C++ | client monitor gate (`perf_pubsub_client.cpp:205-210`) | steady deadline (`:240-272`) | exact frame+header/phase (`:76-113,277-290`) | guard 안 (`:293`) | 같은 유효 집합 (`:294-296`) | 공통 printer (`perf_common.hpp:834-843`) |
| .NET | client monitor+START gate (`PerfMultiPubSubClient.cs:43-61`) | Stopwatch deadline (`:100-118`) | exact frame/header/recv deadline (`:130-157`) | guard 안 (`:159`) | 같은 `recvTicks` (`:161-165`) | 공통 printer (`PerfShared.cs:171-179`) |
| Java | client monitor+START gate (`PerfMultiPubSub.java:109-123`) | `nanoTime` deadline (`:123-136`) | common exact header/phase (`PerfMetricHeader.java:61-73,84-102`) | `recordNanos` 안 (`PerfMetricsCollector.java:41-53`) | 같은 `receivedAt` (`PerfMetricHeader.java:68-71`) | 공통 formatter (`PerfReport.java:26-44`) |
| Rust | client monitor gate (`perf_multi_pubsub_client.rs:72-80`) | Instant deadline (`:111-126`) | exact header+deadline (`:25-38`) | guard 안 (`:40`) | 같은 유효 집합 (`:41-42`) | 공통 printer (`perf_common.rs:763-792`) |
| Go | client monitor gate (`perf_multi_pubsub.go:121-123`) | 공통 deadline (`:147-179`) | exact header+deadline (`:229-255`) | guard 안 (`:258`) | 같은 유효 집합 (`:260-262`) | 공통 printer (`common.go:161-166`) |
| Node | client connect+START gate (`perf_multi_pubsub_client.ts:56-77`) | monotonic start/stop (`:77-108`) | collector exact header+window (`perf_measurement.ts:344-365`) | guard 안 (`:366`) | 같은 유효 집합 (`:367`) | 공통 formatter (`:215-228`) |
| Python | monitor+START gate (`perf_multi_pubsub_client.py:40-68`) | `perf_counter` deadline (`:68-93`) | common exact header+deadline (`perf_metrics.py:114-139`) | guard 안 (`perf_multi_pubsub_client.py:123`) | 같은 유효 집합 (`:124-125`) | 공통 printer (`perf_metrics.py:273-277`) |

대조 결과, 여섯 anchor의 순서와 집계 집합이 C와 어긋나는 잔여 항목은 없다.
single request/reply도 completion deadline guard 안에서 count와 latency를 함께 확정하며,
.NET의 위치는 §2.2와 같이 C와 동일하다.

## 4. 검증

### 4.1 정적/빌드/단위 검증

- `git diff --check`: 통과.
- C report policy test: single 17개, multi 32개 통과. C `perf_stream_client`
  target도 재빌드했다.
- C++: 변경한 single one-way target과 multi 전체 target 빌드 통과.
- .NET: single/multi `dotnet build` 통과(경고/오류 0).
- Java: `:perf-single:classes :perf-multi:classes` 통과.
- Rust: pinned runtime으로 single/multi `cargo test` 통과. 기존 dead-code 경고만 있다.
- Go: `go test ./...` 통과.
- Node: repository TypeScript `tsc --noEmit` 통과.
- Python: pinned 0.17.1 header/lib로 C extension을 별도 빌드했고 `compileall` 통과.
  bundled 0.17.0 파일은 바꾸지 않았다.
- 변경 shell script `bash -n`: 통과.

### 4.2 status-only smoke

모든 실행 전에 `scripts/perf/wait-for-idle-perf.sh`를 통과시켰고 한 번에 하나만
실행했다. 공통 조건은 `PERF_FAIL_FAST=1`, duration 1, runs 1, size 64, tcp다.

| suite | C | C++ | .NET | Java | Rust | Go | Node | Python |
|---|---|---|---|---|---|---|---|---|
| single PAIR | complete | complete | complete | complete | complete | complete | complete | complete |
| multi MULTI_DEALER_DEALER | complete | complete | complete | complete | complete | complete | complete | complete |

- C++ multi 변경 target은 `MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER,MULTI_PUBSUB`
  추가 smoke에서도 3/3 complete였다.
- C `MULTI_STREAM` 직접 reporter도 추가 smoke에서 complete였고 latency 세 metric이
  6자리로 출력됐다.
- .NET single은 `PERF_SINGLE_LATENCY_SAMPLE_CAP=0` 추가 smoke도 complete였고,
  RESULT에서 p95/p99가 mean과 같았다.
- 16개 기본 smoke의 RESULT 전 줄을 검사해 3/6자리 위반 0건을 확인했다.
- 위 실행의 처리량 값은 정합 판정이나 개선 근거로 사용하지 않는다.

## 5. 변경 범위와 작업 분리

- 변경은 perf runner/source와 생성된 Node `dist-tools/perf` 산출물에만 있다.
- `bindings/<lang>/src` binding library와 Core는 수정하지 않았다.
- 작업 시작 전부터 있던 framework grpc smoke 디렉터리 3개는 건드리지 않았다.
- 작업 중 별도 변경으로 나타난
  `bindings-library-performance-improvement-plan-core-0.17.0.ko.md`의 6b/6c 행은
  이 작업의 변경이 아니며 수정하거나 되돌리지 않았다.
