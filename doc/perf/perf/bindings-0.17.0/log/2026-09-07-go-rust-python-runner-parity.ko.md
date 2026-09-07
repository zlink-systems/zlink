# Go·Rust·Python perf 러너 정책 정합 — 2026-09-07

> 상태: **Rust single/multi와 Python single/multi 정합 완료. Go single/multi REQREP은
> 공개 binding terminal의 동일 socket 동시 request가 admission 오류를 반환하여 동적 검증 차단.**
> 수정 범위는 `bindings/{go,rust,python}/perf`뿐이다. binding 라이브러리 소스와 보호 문서는
> 수정하지 않았다. branch 전환·commit·push도 하지 않았다.

## 0. 판정 기준과 실행 조건

- 유일한 판정 기준은 `doc/perf` 정책이다(D-BP2 `decisions.ko.md:1725-1729`,
  D-BP11 `:1842-1844`).
- single의 동기 모델은 in-flight 1이 아니라 러너 전용 OS thread의 진행 소유를 뜻한다
  (`PERF_SINGLE_TEST_POLICY.md:53-67`). admission과 reply는 별개 사건이고(`:69-83`),
  연속 제출과 completion drain이 기준이다(`:85-128`). awaitable 타입은 허용하지만 측정 진행을
  공유 scheduler에 맡기는 것은 금지한다(`:146-167`).
- multi REQREP은 socket별 reply를 기다리지 않고 연속 제출하면서 completion을 함께 진행한다
  (`PERF_MULTI_TEST_POLICY.md:161-184`).
- 모든 smoke와 처리량 측정은 저장소 밖 고정 Core 0.17.1을 사용했다(D-BP7 `:1777-1778`,
  D-BP9 `:1824-1836`).
  - `ZLINK_CORE_SOURCE=release`
  - `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1`
  - `lib/libzlink.so.0.17.1` SHA-256:
    `a3e00fd269b2a1c8d66371ac7ae7efd3f6dd25e6ab842484b847352258ea39a2`
- 측정 조건: `DEALER_ROUTER_REQREP`, `tcp`, 64 B, active 2 s, 1 run,
  `PERF_SINGLE_REQREP_MAX_OUTSTANDING=64`(기본값).

## 1. 변경 결과

| 요구 | Go | Rust | Python | 정책 근거 |
|---|---|---|---|---|
| A. monotonic 시간원 | metric header·수신 판정은 host-wide `CLOCK_MONOTONIC`/QPC. 프로세스 내부 deadline은 monotonic component가 든 `time.Time` | header·수신 판정은 `clock_gettime(CLOCK_MONOTONIC)`, deadline은 `Instant` | header·수신 판정은 `time.monotonic_ns()`, deadline은 `time.perf_counter()` | `PERF_POLICY.md:128-140` |
| B. 메시지당 환경 조회 캐시 | `PERF_PART_COUNT`를 package 초기화 때 1회 읽음 | `OnceLock`으로 `PERF_PART_COUNT` 1회 읽음 | module 상수로 `PERF_PART_COUNT` 1회 읽음 | C `perf_zlink_part_helpers.hpp` 함수 scope static 준용 |
| C. REQREP 미완료 상한 | single/multi 모두 기본 64, 하한 2 | 〃 | 〃 | `PERF_SINGLE_TEST_POLICY.md:112-128`, `PERF_MULTI_TEST_POLICY.md:161-184` |
| D. single REQREP | 최대 64개 requester goroutine을 각각 `LockOSThread`로 구성했으나 두 번째 동시 submit이 `SubmitBackpressured(EAGAIN)`을 terminal result로 반환. 오류 은폐 제거 후 fail | 한 OS thread에서 Future를 직접 poll하고 `POLLOUT|POLLCOMPLETION`을 진행; fill/refill 후 bounded drain | requester `threading.Thread`가 자기 private asyncio loop와 poller를 소유; capped task fill/refill 후 drain | `PERF_SINGLE_TEST_POLICY.md:85-128,136-139,146-167` |
| E. multi REQREP | socket별 cap까지 goroutine을 제출하고 먼저 끝난 completion부터 refill/drain. 실제 Core 0.17.1 실행은 동일 socket 두 번째 request에서 `EINVAL` | socket별 capped Future 집합 fill/refill/drain | socket별 capped Task 집합 fill/refill/drain | `PERF_MULTI_TEST_POLICY.md:161-184` |
| F-1. Go send drain | `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS`, 기본 5000 ms를 DEALER_DEALER·SENDSEND·REQREP drain에 적용 | 기존 의미 유지 | 기존 의미 유지 | `PERF_MULTI_TEST_POLICY.md` §12.3 |
| F-2. Auto-HWM | 첫 유효 메시지 뒤 새 monitor snapshot을 열어 C 형식 출력 | 〃 | 〃 | `PERF_MULTI_TEST_POLICY.md:301-323` |
| F-3. transport 선택 | 사용자 `--transports` 순서 보존, unsupported 제거, 중복 제거, `CONTROL_PLANE_PATTERNS` 분기 이식 | 〃 | 〃 | C runner와 CLI 의미 일치: `PERF_POLICY.md:178-187` |
| F-4. Effective Options | C key 집합으로 정리하고 REQREP일 때 해당 single/multi cap 노출 | 〃 | 〃 | `PERF_POLICY.md:178-182` |

주요 구현 위치:

- Go: `internal/perfcommon/monotonic.go:1-32`, `measurement.go:24-35,80-93`,
  `single/perf_reqrep.go:16-90`, `multi/perf_multi_socket_reqrep.go:206-333`,
  `multi/perf_multi_main.go:22-40`, `internal/perfcommon/common.go:180-231`,
  `run_benchmarks_multi.sh:131,757-765,1001-1007`.
- Rust: `single/src/common.rs:204-212,748-850`,
  `multi/src/perf_common.rs:334-343,365-374,818-911,1219-1241`,
  `multi/src/perf_multi_socket_reqrep.rs:240-340`,
  `run_benchmarks_multi.sh:80,142-150`.
- Python: `perf_metrics.py:70-77,114-130,140-153`,
  `single/perf_common.py:54-56,164-170`, `single/perf_single_reqrep.py:117-247`,
  `multi/perf_multi_common.py:54-65,65-132,341-368`,
  `multi/perf_multi_reqrep_client.py:60-75,165-224`,
  `multi/run_benchmarks.py:171-187,1320-1326`.

Python multi의 기본 I/O thread도 C/정책의 4로 바로잡았다
(`PERF_MULTI_TEST_POLICY.md:301-310`, `perf_multi_common.py:58-60`).

### 1.1 규칙 수 변화

수정 전에는 언어마다 별도의 in-flight 1 RTT loop 세 개와 multi 직렬화 규칙이 있었다.
수정 후에는 **cap까지 fill → 완료 순 drain → 즉시 refill**이라는 정책 규칙 하나만 남겼다.
즉 설명해야 할 부하 규칙은 **4개 → 1개**다. Go에도 같은 규칙을 적용했지만 공개 blocking
terminal이 두 번째 동시 request를 받아들이지 않아 실행은 fail한다. 러너에 별도 retry/timer
상태를 추가해 하위 오류를 우회하지 않았다.

## 2. 시간원 선택과 cross-process 조건

| 언어 | 선택 API | 같은 호스트 프로세스의 공유 기준점 근거 | 적용 |
|---|---|---|---|
| Go | Unix: POSIX `clock_gettime(CLOCK_MONOTONIC)`, Windows: `QueryPerformanceCounter` | Go `time.Time`의 monotonic reading은 문서상 다른 프로세스에서 의미가 없으므로 header에 쓰지 않았다. Linux `CLOCK_MONOTONIC`은 boot 이후의 nonsettable system-wide clock이며 wall-clock jump 영향을 받지 않는다. QPC는 system time과 독립인 시스템 성능 카운터다. | `sent_ts_ns`, receive/completion 판정은 `MonotonicNowNs`; elapsed/deadline은 monotonic component를 보존한 `time.Time` |
| Rust | POSIX `clock_gettime(CLOCK_MONOTONIC)` | Linux에서 동일 host-wide boot 축이다. `Instant`도 프로세스 내부 elapsed/deadline에만 쓰므로 직렬화 문제가 없다. | single/multi `now_ns`; deadline/timeout/drain은 `Instant` |
| Python | `time.monotonic_ns()` | Python 문서는 monotonic clock이 system clock 갱신의 영향을 받지 않고, Unix에서는 `CLOCK_MONOTONIC`이며 3.5부터 모든 프로세스에서 같은 clock이라고 명시한다. | header·수신 판정은 `monotonic_ns`; elapsed/deadline은 같은 monotonic 계열 `perf_counter` |

문서 근거:

- Go `time`: <https://pkg.go.dev/time> — `Time`의 monotonic component와 프로세스 밖에서의
  무의미함을 함께 명시한다.
- Python `time`: <https://docs.python.org/3/library/time.html#time.monotonic> — non-adjustable
  monotonic clock, 동일 프로세스뿐 아니라 모든 프로세스에서 같은 clock.
- POSIX/Linux `clock_gettime`: <https://man7.org/linux/man-pages/man3/clock_gettime.3.html> —
  `CLOCK_MONOTONIC`은 설정할 수 없는 system-wide clock이며 discontinuous wall-clock 변화의
  영향을 받지 않는다.
- Rust `libc::clock_gettime`: <https://docs.rs/libc/latest/libc/fn.clock_gettime.html>.
- Windows QPC: <https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps>.

`time.time_ns()`는 Python multi IPC 임시 파일 이름 생성에 한 번 남아 있다
(`multi/perf_multi_common.py`). 결과 표기용 식별자이며 경과 시간, deadline, timeout, drain,
metric header 또는 수신 판정에는 관여하지 않는다.

## 3. single REQREP 처리량 before/after

| 언어 | before (ops/s) | after (ops/s) | 배수 | 완료 상태 |
|---|---:|---:|---:|---|
| Go | 9,202.5 | **유효 결과 없음** | — | cap 2/64 모두 `SubmitBackpressured(EAGAIN)`으로 fail |
| Rust | 6,540.0 | 357,654.5 | 54.69× | complete, RESULT 5/5 |
| Python | 6,199.5 | 17,916.5 | 2.89× | complete, RESULT 5/5 |

Go에서 한때 관측된 49,764.5 ops/s는 concurrent submit 오류를 모두 `continue`로 삼키던
경로의 값이라 폐기했다. 오류 은폐를 제거한 현재 러너는 RESULT를 내지 않는다. 비교 기준 C는
같은 `tcp` 64 B에서 841,588 ops/s다. Python은 RTT 1의 수천 ops/s 영역은 벗어났지만
C++/.NET/JVM 수준에는 못 미친다. 이번 작업의 합격 기준은 성능 목표치가 아니라 정책 정합이며,
추가 성능 개선은 별도 pass로 분리한다.

## 4. 측정 anchor 6종 대조

`PERF_POLICY.md:147-156`은 여섯 측정 anchor와 RESULT 확정 지점을 열거한다. 사용자가 요구한
6종은 RESULT 직전의 측정 anchor로 대조하고, RESULT 확정은 별도 행에 적었다.

| anchor | C 의미 | Go | Rust | Python |
|---|---|---|---|---|
| send timestamp | terminal 호출 직전 active payload에 monotonic stamp | terminal을 부르는 goroutine 안에서 stamp | submit Future를 만들기 직전에 `encode_header` | eager task 생성 직전 stamp; task가 같은 전용 thread에서 즉시 terminal 진행 |
| ready 만족 | transport 연결/구독/stream ready가 성립한 뒤 active 진입 | 기존 C handshake 유지 | 기존 C handshake 유지 | 기존 C handshake 유지 |
| active 시작·종료 | ready 뒤 monotonic deadline; 종료 뒤 신규 제출 없음 | monotonic `time.Time` window | `Instant` deadline | `perf_counter` deadline |
| 유효 recv/completion | header magic/run/phase/size/part shape와 active 종료 전 완료 확인 | `MeasurementPayload` + completion timestamp | `record_reqrep_completion`/message validator | `measurement_payload` + decoded header + completion timestamp |
| throughput count | active 유효 recv/reply completion 뒤 1회 증가 | `AddCount`/`AddLatencyNs`가 유효 판정 뒤 실행 | `record_ns`가 유효 active completion 뒤 실행 | `completed += 1`이 유효 active completion 뒤 실행 |
| latency sample | 유효 record당 1회; REQREP RTT는 `/2`, one-way는 cross-process delta | monotonic delta `/2` 또는 one-way delta | 〃 | 〃 |
| RESULT 확정 | drain/집계가 끝난 뒤 필수 5 metric을 한 번 확정 | `FinalizeResult` 후 출력 | result/report 확정 후 출력 | `result_metrics`/report 확정 후 출력 |

처리량 단위와 sampling divisor도 C와 같다. REQREP은 client completion에서 `ops/s`를 세고
RTT를 2로 나눈다(`PERF_MULTI_TEST_POLICY.md:625-640,675-698`).

## 5. Auto-HWM·multi smoke

Core raw option 기본값을 잘못 읽지 않도록 pipe attach/첫 유효 메시지 전에 snapshot을 찍지 않는다.
`MULTI_DEALER_DEALER`, tcp 64 B smoke에서 세 언어 모두 client/server snapshot이
`sndhwm=1048576`, `rcvhwm=1048576`으로 출력되었고 결과가 complete였다.

| 언어 | throughput | Auto-HWM client/server | 상태 |
|---|---:|---|---|
| Go | 948,477 msg/s | 1,048,576 / 1,048,576 | complete |
| Rust | 1,079,553 msg/s | 1,048,576 / 1,048,576 | complete |
| Python | 128,770 msg/s | 1,048,576 / 1,048,576 | complete |

## 6. 검증

| 검증 | 결과 |
|---|---|
| Go `go test ./perf/...` (고정 Core env) | 통과 |
| Go single REQREP runner 재실행 | partial: one-way 5종 complete, REQREP 2종 fail |
| Go single REQREP direct, cap 2/64 | 모두 `SubmitBackpressured`, errno `EAGAIN`, exit 1 |
| Rust single `cargo fmt --all -- --check`, `cargo check` | 통과(기존 dead-code warning만) |
| Rust multi `cargo fmt --all -- --check`, `cargo check` | 통과(기존 dead-code warning만) |
| Python `python3 -m compileall -q bindings/python/perf` | 통과 |
| Go/Rust `run_benchmarks*.sh` `bash -n` | 통과 |
| `git diff --check` | 통과 |
| Rust multi REQREP smoke | complete, 93,596 ops/s |
| Python multi REQREP smoke | complete, 약 29,664 ops/s |
| Go multi REQREP direct public-API repro | 실패: 두 번째 동시 request admission에서 `SubmitInvalidArgument`, errno `EINVAL`, exit 1 |

full pattern/transport/size matrix는 실행하지 않았다. 관련 unit/type/syntax 검증과 single REQREP,
multi REQREP, Auto-HWM one-way smoke까지만 수행했다.

## 7. 남은 항목과 감독자 판단

### 7.1 Go single/multi REQREP — perf 범위 밖 하위 계층 결함

`PERF_MULTI_REQREP_MAX_OUTSTANDING=2`, requester socket 1개, 고정 Core 0.17.1의 직접 실행에서도
첫 request가 진행 중인 같은 socket에 두 번째 public `RequestSubmitOp.Submit`을 호출하면 즉시
`SubmitInvalidArgument(EINVAL)`이 발생한다. 공개 계약은
`RequestSubmitOp.Submit(context.Context) ([]*Message, error)` 하나뿐이고
(`bindings/go/internal/native/operations.go:23-32`, `bindings/go/contracts/sockets.go:41-45`),
별도 async/admission terminal은 surface에서 제공하지 않는다.

러너에서 reply 뒤 다음 요청으로 직렬화하면 정책 위반이고, multipart를 1 part로 낮추거나
오류를 삼키면 재현을 은폐한다. binding `bindings/go/src`/`internal` 수정은 사용자 범위 밖이므로
러너는 non-backpressure admission 오류를 fatal로 유지했다. 감독자는 이 항목을 **binding/Core
기존 결함 조사로 분리**해야 한다. 그 결함이 해결되기 전에는 Go multi REQREP을 정책 합격으로
판정할 수 없다.

single도 같은 공개 terminal을 여러 `LockOSThread` requester에서 호출한다. cap 2와 64의
직접 실행은 모두 두 번째 동시 요청에서 `SubmitBackpressured(EAGAIN)`을 terminal 오류로
반환했다. 이 terminal은 계약상 backpressure가 나면 exact WRITABLE token에서 같은 요청을
내부 재개해야 하므로(`bindings/go/contracts/sockets.go:43-45`), 러너가 새 요청으로 busy retry할
수 없다. 기존 러너의 모든-error `continue`를 제거하고 request timeout만 명시적으로 분류했다
(`single/perf_reqrep.go:70-76`). 따라서 Go single도 하위 계약이 해결되기 전에는 정책 합격 및
after 처리량을 낼 수 없다.

### 7.2 Go single의 진행 형태

Go 공개 request terminal은 admission과 reply를 합쳐 blocking 반환하므로 한 thread에서
연속 submit과 직접 completion drain을 표현할 수 없다. 복수 전용 requester thread는 정책이
허용한다(`PERF_SINGLE_TEST_POLICY.md:108-110`, D-BP6 `decisions.ko.md:1769-1774`). 그러나
binding 구현은 public poller가 completion ownership을 가져가지 않으면 내부 runtime goroutine을
시작한다(`bindings/go/internal/native/completion_owner.go:345-365,382-402`). 따라서 현재 perf-only
surface로는 사용자가 요구한 “하나의 전용 thread가 submit·poll·집계를 모두 수행”하는 형태를
구성할 수 없다. 단일-thread 직접 drain을 강제하려면 Go 공개 API/terminal 계약 변경이
필요하므로 감독자 확인이 필요하다.

### 7.3 변경하지 않은 것

- binding 라이브러리 소스, Core, Framework, 정책/스펙 문서
- 기존 사용자 untracked `framework/languages/{cpp,node}/bench/with-grpc/log/smoke*`
- branch, commit, push
