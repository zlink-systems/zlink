# C++ single REQREP runner parity 조사·검증

결과: **BLOCKED — 구현 미완료, 소스 변경 없음, EXIT:2**. 현재 공개 C++ request API로는 C의 admission backpressure 경계를 관찰할 수 없다. 사용자 지정 수정 범위에서는 정확한 parity를 만들 수 없어, 무제한 async 제출이나 runner window를 추가하지 않았다. 지정 벤치는 **변경 전 baseline**으로만 실행했다.

- 작업 트리: `/home/hep7hep7/project/zlink-wt-cpp-single-reqrep`, detached HEAD `482549a009`.
- 기존 untracked `core/build`, `core/build-dev` symlink 유지. git branch 변경·commit·push·reset·checkout·stash 및 Core configure/build/clean 미실행.
- C++ binding build는 기존 Core shared library를 IMPORTED target으로 연결했다. `CMAKE_BUILD_PARALLEL_LEVEL=3`, `ZLINK_BUILD_JOBS=3` 적용.

## 차이 표

| 항목 | 현재 C | 현재 C++ | 판정 |
|---|---|---|---|
| 처리량 제출 | HWM backpressure까지 제출, 64건마다 nonblocking completion poll, 막히면 50ms completion wait | 매 요청 `.submit()`으로 reply까지 blocking | 불일치 |
| completion 소유자 | requester thread의 submit/poll 교대 | requester의 synchronous 결과 처리 + 별도 completion thread | 불일치 |
| 완료 집계 | active deadline 전 완료된 정상 왕복 / duration | active deadline 전 완료된 정상 왕복 / duration | 공식 일치, 제출 모델 불일치 |
| latency | throughput drain 후 별도 1초, **같은 포화 제출 모델** | throughput과 같은 단계에서 모든 정상 reply latency 집계 | 불일치; 요청문의 직렬 latency 설명도 C와 다름 |
| timeout / drain | request 200ms, bounded completion drain 10000ms 기본 | 같은 기본값 | 일치 |
| 2 parts | payload + empty tail, reply shape 검사 | payload + empty tail, reply shape 검사 | 의미 일치 |
| stop | wire `__zlink_perf_stop__` | 같은 wire token | 일치 |
| RESULT | 7 CSV fields, throughput·bandwidth·latency·p95·p99; ops/s, MB/s, ms | 같은 fields·metrics·units·왕복 bandwidth 배수 2 | 구조 일치; C latency 소수 6자리, C++ 3자리 |
| runs 집계 | metric별 median 정책 | `single/run_comparison.py:1390–1394` metric별 median | 일치 |

C 근거: `bindings/c/perf/single/common/perf_single_reqrep.hpp:169–205,421–449,451–467,537–548`.
C++ 근거: `bindings/cpp/perf/single/common/perf_single_reqrep.hpp:45–81,95–112,180–205,296–399`.

## API 제약과 대안

1. `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:305–327`: request terminal은 `.async()`와 `.submit()`이다. request builder에 공개 DONTWAIT flags/try-submit/callback terminal이 없다.
2. `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:74–97`: `.submit()`은 `entry->wait_request()`까지 기다린다. 현재 러너 직렬화의 직접 원인이다.
3. 같은 파일 `:100–141`: `.async()`가 내부 DONTWAIT를 설정하지만 결과는 reply의 `async_result_t`뿐이다.
4. `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:225–229`, `completion_owner.cpp:254–267,286–290`: 정상 BACKPRESSURED는 예외가 아니라 내부 false이며, binding이 정확한 WRITABLE token으로 보관·재제출한다. `start_request()`는 admission 반환값을 버린다.
5. `operation_contracts.hpp:61–104,128–140`: awaiter의 readiness는 **reply 완료 여부**다. admission 여부가 아니다. coroutine 없이 awaiter를 직접 관찰하더라도 HWM backpressure 여부는 알 수 없다.
6. 기존 공개 API repro: `bindings/cpp/tests/contract/test_cpp_contract_request_writable_retry.cpp:134–199`는 작은 HWM에서 32개 `.async()`가 모두 반환되고, 일부만 처음 admit된 뒤 public poller가 나머지를 재제출·완료시킴을 검증한다. 해당 test 통과.

| 대안 | 결과 |
|---|---|
| 공개 async 연속 호출 + 64건마다 poll | backpressure가 러너까지 전달되지 않아 C처럼 제출 중단 불가; binding 대기 요청이 계속 쌓임. 채택하지 않음 |
| 매 async마다 reply readiness 대기 / 고정 window | 직렬화 또는 새 in-flight 상한으로 사용자 금지 조건 위반. 채택하지 않음 |
| C API/private socket 접근 | C++ public 경로를 우회하며 completion 소유권을 중복 구현. 채택하지 않음 |
| 공개 admission 결과와 completion을 분리하는 계약 검토 | 정확한 포화 경계를 표현할 후보이나 library·public header 변경 금지 범위 밖. 구현·새 API 제안 확정 없이 blocker로 보고 |

소유 계층: admission·WRITABLE 재제출은 Core/C++ binding, phase·측정은 perf runner.
변경 분류: 구현 없음. **D — perf 정책 요구와 공개 C++ API 표현 능력 사이 gap**. Framework runtime 변경이 아니므로 Framework 승인 단계는 적용 대상이 아니다.
수정 전/후 규칙 수: 소스 무변경으로 동일; 새 상태·상한·spin·sleep·재시도 규칙 0개.

## 검증 표

| 검증 | 결과 | 근거 |
|---|---|---|
| 사용자 지정 tcp / 두 REQREP / 64,1024,65536 / 5초 / runs 1 명령 | baseline 6/6 complete, fail 0, exit 0 | `cpp-single-reqrep-runner-baseline.log` |
| RESULT 구조·필수 metric·왕복 bandwidth 공식 | 30/30, 7 fields, 5 metrics, 배수 2 확인 | 저장 report를 Python으로 검사 |
| `test_cpp_contract_request_writable_retry` | PASS | `cpp-single-reqrep-runner-tests.log` |
| `test_cpp_perf_application_ready_queue` | PASS | 같은 test log |
| `git diff --check` | PASS | 소스 diff 없음 |
| 수정 후 parity gate | 미실행/미충족 | API blocker로 수정 없음 |

실행 명령:
```sh
ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=3 PERF_TRANSPORTS=tcp bash bindings/cpp/perf/run_benchmarks.sh --pattern DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP --msg-sizes 64,1024,65536 --duration 5 --runs 1
```
추가 환경 `CMAKE_BUILD_PARALLEL_LEVEL=3`. 시작 uptime load1=0.08, 종료=0.33. 감독 프로세스가 5초 간격으로 load1>3 여부를 확인하고 초과 시 프로세스 그룹을 종료하도록 실행했다. 초과 감지 없음. 시작·종료 uptime은 진행 로그에 기록했다. 세부 load 샘플은 별도 저장하지 않았다.

저장 report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_211611.txt`.

## C 기준 대비 참고 비율

이번 C++ 값은 **수정 전 baseline**이다. C 측정 시각·Core artifact가 다르며 실행 모델도 다르므로 library 비용 비교나 개선 효과로 해석할 수 없다.
C 원본은 이 worktree에 없어서 `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/`에서 읽었다. DEALER는 `064250_p1cpp-single`, ROUTER는 `070313_p1cpp-single`이다. 후자는 지정한 07:00 이전 범위를 벗어나지만 pass1 요약에 사용된 값과 일치한다. `065502` 파일은 ROUTER_ROUTER one-way이므로 REQREP 기준으로 사용하지 않았다.

| Pattern (tcp) | Size B | C ops/s | C++ baseline ops/s | C++ / C |
|---|---:|---:|---:|---:|
| DEALER_ROUTER_REQREP | 64 | 391,337.400 | 8,498.800 | 2.17% |
| DEALER_ROUTER_REQREP | 1024 | 398,052.600 | 8,393.200 | 2.11% |
| DEALER_ROUTER_REQREP | 65536 | 7,675.800 | 6,395.000 | 83.31% |
| ROUTER_ROUTER_REQREP | 64 | 443,002.600 | 8,529.000 | 1.93% |
| ROUTER_ROUTER_REQREP | 1024 | 382,420.000 | 8,233.800 | 2.15% |
| ROUTER_ROUTER_REQREP | 65536 | 7,806.800 | 6,444.400 | 82.55% |

## 다른 binding 러너 정적 조사

모든 경로는 현재 worktree 기준이며 수정·벤치 실행하지 않았다. 표의 완료 집계는 응답 이후 집계를 뜻하며, deadline 검사 차이는 별도로 표시한다.

| Binding | 포화 제출 / HWM 중단 | 완료 왕복 집계 | 별도 1초 latency | 코드 근거·차이 |
|---|---|---|---|---|
| dotnet | Async Task 연속 제출, 64건 poll/HWM 중단의 C 모델은 아님 | 예, deadline 전 완료 | 없음 | `bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs:406–556`; 별도 poll thread와 async Task continuation, 같은 단계 latency |
| java | 아니오, submit_sync 직렬 | 예, deadline 확인 | 없음 | `bindings/java/perf/single/Zlink.BindingBench/src/main/java/systems/zlink/perf/single/PerfSocketReqRep.java:107–169` |
| node | 아니오, submit_sync 직렬 | 응답을 collector에 전달 | 없음 | `bindings/node/perf/single/perf_socket_reqrep.ts:110–139`; activeStopNs가 collector에 전달됨 |
| go | 아니오, Submit(ctx) 직렬 | 예, **reply 뒤 deadline 재검사 없음** | 없음 | `bindings/go/perf/single/perf_reqrep.go:15–69` |
| rust | 아니오, submit_sync 직렬 | 예, **reply 뒤 deadline 재검사 없음** | 없음 | `bindings/rust/perf/single/src/common.rs:715–769`; `perf_dealer_router_reqrep.rs:78–87`, `perf_router_router_reqrep.rs:96` 이하 |
| python | 아니오, submit_sync 직렬 | 예, deadline 확인 | 없음 | `bindings/python/perf/single/perf_single_reqrep.py:90,110–157` |

## spec/정책 gap 여부

**있음.** `doc/perf/PERF_SINGLE_TEST_POLICY.md:37,137–140,293`은 같은 active 구간의 동일 메시지로 throughput과 latency를 계산한다고 명시하지만, 현재 C `:537–548`은 별도 1초 latency 단계를 실행한다. C의 두 단계는 모두 포화 제출이며 요청문의 “별도 1초 단계(요청 1건씩 왕복)”와도 다르다. 정책 `:51–66,112–123`의 synchronous callback/completion 모델 역시 현재 C++ 공개 terminal에는 직접 대응하지 않는다. 정책·spec은 수정하지 않았다.

## BLOCKERS

- **B1 필수:** 정확한 HWM 제출 중단을 표현하는 공개 C++ API가 없어 runner-only 범위로 목표 달성 불가. library·공개 헤더를 변경하지 않고서는 위 제약을 해소할 수 있는 경로를 찾지 못했다.
- **B2 기준:** C 코드(별도 포화 latency), 정책(동일 구간), 요청문(별도 직렬 latency)의 기준 불일치. 비동기 질문으로 확인 요청했으나 요약 작성 시 응답 없음.
- **B3 참고 비교:** ROUTER의 확인된 C 원본 시각은 07:03:13으로 엄격한 07:00 이전 조건 밖이다. 기존 pass1 표와 같은 참고값으로만 제공했다.

변경 파일: repository source 없음. 생성 산출물은 지정 진행 로그·본 요약·baseline/test 로그 및 runner가 생성한 ignored build/runtime/report이다. 남은 test 실패 없음; 구현·parity 목표는 미완료.
