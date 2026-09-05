# C++ Single REQREP hot-path pass 1

- 작성: 2026-09-05T21:08:48.332151+09:00
- 판정: **no-go — library와 runner의 추적 파일 변경 0개**. 공개 API·ownership·error contract·측정 의미를 유지한 채 채택할 후보를 찾지 못했다.
- 범위: detached HEAD `482549a009`, `bindings/cpp/**`만 사용. Core build·clean·checkout·commit·push 미실행. Core 0.17.0 SHA-256 `d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00`.
- 규칙 수: 수정 전/후 동일(추가·삭제 0).

## 원인 표

| 항목 | 근거 | 판단 |
|---|---|---|
| 요청 제출과 대기 | `bindings/cpp/perf/single/common/perf_single_reqrep.hpp:177-205`, `request_reply.cpp:74-97`, `completion_owner.cpp:513-523` | blocking `submit()` → entry condition variable. future·async await·scheduler를 사용하지 않는다. |
| 완료 진행 | `bindings/cpp/src/Runtime/Eventing/poller.cpp:520-529`, `completion_owner.cpp:630-690,484-504` | public poller의 wait thread가 drain·capture·직접 notify. 런타임 completion thread를 추가로 거치지 않는다. |
| C/C++ 러너 차이 | C `perf_single_reqrep.hpp:421-449,537-548`, C++ `perf_single_reqrep.hpp:333-381` | C는 HWM backpressure까지 포화 제출하며 64건마다 completion poll; 처리량 단계 뒤 별도 1초 latency 단계. C++는 요청 1건마다 reply를 기다리고 같은 단계에서 latency를 기록. C도 serial이라는 전제는 현재 소스와 다르다. |
| fixed sleep 가설 | C++ public wait timeout 50ms, fallback 25ms; runtime fallback은 public owner 등록 시 정지(`completion_owner.cpp:788-790`) | timeout은 event가 없을 때의 최대 대기다. 완료가 온 뒤 1ms tick을 기다리는 경로는 없다. 100µs대 요청 간격을 25/50ms timeout으로 설명할 수 없다. |
| 잔여 할당·registry | `request_reply.cpp:83`, `completion_owner.cpp:535-559`, `operation_state.hpp:304-345` | blocking entry는 요청당 새 shared object. 첫 registry entry는 이미 inline, map node는 PMR pool, builder state는 thread-local pool이다. reply vector는 반환값의 ownership을 가진다. |

## 계약과 후보 판정

- 소유 계층: binding completion owner가 public poller와 runtime fallback 사이 drain ownership을 관리하며 Core가 request/reply lifecycle을 소유한다.
- Spec: `bindings/doc/spec/cpp/README.ko.md:666-674` — provisional registration, publish/capture join, public poller 소유 시 다른 wait thread 필요. Core 공개 헤더 `core/include/zlink/socket/api.h:252-283` — REQUEST FINAL admission·reply token ownership.
- 교차언어: C 러너는 같은 thread에서 submit과 completion drain을 수행하지만 여러 요청이 outstanding이다. C++ public blocking terminal의 thread handoff와 C의 포화 제출 효과를 분리해야 한다. Framework runtime은 조사·수정하지 않았다.
- 변경 분류: 채택 변경 없음(no-go). library 결함 수정 또는 계약 적응으로 분류할 diff가 없다. 대상 completion ownership에 새 spec gap은 확인되지 않았다.

| 후보 | 가이드 | 판정 |
|---|---|---|
| 추가 scheduler 제거·direct completion | §2.3 | 이미 direct notify다. 제거할 binding scheduler hop 없음. |
| 완료 대기 sleep 제거 | §2.3 | 실제 event 기반 wait. spin·timeout-0 반복·timeout 변경은 시도하지 않음. |
| 요청 thread가 직접 completion drain | §2.3 + C++ public poller 계약 | public drainer와 충돌하거나 ownership 정책을 바꾼다. no-go. |
| completion entry pool/stack 재사용 | §4 callback userdata 재사용 기각 | Core가 entry 주소를 user_context로 보유하며 completion thread가 waiter 재개 뒤에도 entry를 참조한다. 재사용 시 lifetime/ABA 위험; 시도하지 않음. |
| registry inline·builder pool·reply adopt | §2.1/2.4 | 기존 pass에 반영됨. 중복 pool·mapping 추가 안 함. |
| sync settle 경로 lock 미세 정리 | §1 효과 기준 | completion→notify 구간 자체가 sub-µs 수준이며 추가 hop을 없애지 못함. 측정한 병목의 근본 수정 후보로 채택하지 않음. |
| C++ 러너를 async 포화 제출로 전환 | library/runner 효과 분리 규칙 | public async API는 있지만 runner의 outstanding depth와 latency 의미를 바꾸므로 요청 범위에서 제외. |

## 측정 방법과 한계

- 공통: tcp, 64B, part-count 2, I/O thread 1, auto-HWM balanced, 1초. C는 공식 러너가 별도 latency 1초를 추가한다.
- callgrind: `~/.local/bin/valgrind --tool=callgrind --separate-threads=yes --fair-sched=yes`. 기본 Valgrind 스케줄링에서 C가 측정 구간 내 completion 부족으로 실패해 공정 스케줄링으로 진단했다. timeout·budget·HWM·assertion은 변경하지 않았다.
- 명령 수 분모: 실행 전체의 FINAL reply 수(`zlink_reply_part` 호출 / 2). 시작·종료 비용 포함. C의 두 측정 단계와 C++의 직렬 실행 차이가 포함되므로 순수 library 증분으로 해석하지 않는다.
- 시간 계측: 임시 LD_PRELOAD interposer가 native FINAL 입출구, 서버 최종 recv/reply, completion 반환, pthread condition wait/notify를 monotonic timestamp로 기록. 첫 native MORE·builder 준비 비용은 이전 wake→다음 FINAL 구간에 포함. 실제 future는 없으므로 waiter 복귀로 resolve를 관찰한다.
- 실제 futex syscall 횟수는 별도 계측하지 않았다. 표의 wait/notify는 pthread condition-variable API 호출 수이며 futex syscall 수와 같다고 가정하지 않는다.
- 초기 diagnostic은 RelWithDebInfo였고 다른 job과의 겹침 가능성이 있어 참고용으로만 보존했다. 최종 `*-release*` diagnostic과 `after-complete`를 판정 자료로 사용한다. 중단한 after 2개는 비교표에서 제외한다.
- 최종 측정 controller는 load1≤3 및 외부 benchmark 없는 60초를 확인하고 시작하며, 실행 중 0.5초마다 load/외부 프로세스를 감시한다. 위반 시 자기 measurement group만 중단한다. 전체 gate는 측정이 끝난 뒤 JOBS=3으로 실행한다.

## 요청당 비용 대조

| 항목 | C | C++ | 해석 |
|---|---:|---:|---|
| 전체 완료 request 수 (profile 분모) | 8,987 | 932 | C의 별도 latency 단계 포함 |
| 전체 Ir / request | 33,875 | 66,784 | 시작·종료 및 Core I/O 포함 |
| native request_part 호출/request | 2.0000 | 2.0000 | 전체 프로세스 API 호출 수 |
| completion_recv 호출/request | 1.0167 | 2.0000 | 전체 프로세스 API 호출 수 |
| public native poller_wait 호출/request | 0.0169 | 1.0021 | 전체 프로세스 API 호출 수 |
| pthread_cond_wait 호출/request | 0.0000 | 0.9989 | 전체 프로세스 API 호출 수 |
| pthread_cond_timedwait 호출/request | 0.0026 | 0.0172 | 전체 프로세스 API 호출 수 |
| pthread_cond_clockwait 호출/request | 0.0179 | 1.0365 | 전체 프로세스 API 호출 수 |
| pthread_cond_signal 호출/request | 0.0003 | 0.0054 | 전체 프로세스 API 호출 수 |
| pthread_cond_broadcast 호출/request | 1.0248 | 3.0933 | 전체 프로세스 API 호출 수 |
| operator new 호출/request | 0.2023 | 6.6727 | 전체 프로세스 API 호출 수 |
| C++ throw 호출/request | 0.0000 | 0.0000 | 전체 프로세스 API 호출 수 |
| binding completion→request thread handoff | 0 | 1 | C는 동일 application thread, C++는 condvar wake |
| binding scheduler/resume queue 왕복 | 0 | 0 | submit() 경로에 coroutine/future가 없음 |
| runner std::function invoke/request | 0 | 1 | C++ callback 관찰 함수, completion scheduler가 아님 |

- C Core Asio read/write `std::function` invoke: 0.0467/request. Transport batching 차이가 포함되며 binding scheduler로 세지 않는다.
- C++ Core Asio read/write `std::function` invoke: 4.0129/request. Transport batching 차이가 포함되며 binding scheduler로 세지 않는다.

| Native 1초 측정 | C | C++ |
|---|---:|---:|
| Throughput ops/s | 485,919 | 8,579 |
| 1/throughput µs | 2.058 | 116.564 |
| 보고 latency mean µs | 19,459.925 | 116.000 |

C의 역수는 포화 처리량당 시간이며 요청 RTT가 아니다. C++는 serial이므로 역수와 RTT가 일치한다.

## 요청 1건 수명 시간 계측

| 구간 | 평균 µs | 중앙값 µs | p95 µs |
|---|---:|---:|---:|
| native FINAL 제출→admission 반환 | 11.847 | 11.131 | 15.350 |
| admission 반환→서버 FINAL 수신 | 37.736 | 34.673 | 56.184 |
| 서버 FINAL 수신→FINAL reply 반환 | 12.247 | 11.372 | 15.786 |
| FINAL reply 반환→client completion 반환 | 35.175 | 31.298 | 52.826 |
| client completion 반환→waiter notify | 0.585 | 0.494 | 0.923 |
| waiter notify→pthread_cond_wait 반환 | 17.024 | 15.570 | 22.764 |
| wait 복귀→다음 요청 native FINAL 제출 | 2.841 | 2.407 | 5.134 |
| 전체 요청 간격 | 117.455 | 108.025 | 175.307 |

- 완결 transition 8,513건. API interposer 자체 오버헤드를 포함한 진단값이다. 최종 native 무계측 값과 같은 100µs대 요청 간격을 확인한다.
- notify 뒤 OS가 대기 thread를 실행하기까지의 시간은 존재하지만, 중간 future/scheduler 단계를 거치는 지연은 아니다. Transport 구간에도 OS scheduling이 포함되어 있어 순수 네트워크 전송 시간으로 분리하지 않는다.

| 예시 요청 transition | 요청 FINAL 대비 µs | TID |
|---|---:|---:|
| native FINAL 진입 | 0.000 | 3678612 |
| admission 반환 | 11.243 | 3678612 |
| 서버 수신 | 47.516 | 3678610 |
| reply 반환 | 59.501 | 3678610 |
| client completion | 90.191 | 3678611 |
| waiter notify | 90.826 | 3678611 |
| waiter 재개 | 105.699 | 3678612 |
| 다음 요청 FINAL | 109.007 | 3678612 |

## Before / after

변경 없는 재측정이다. before는 요청된 2026-09-05 오전 `p1cpp-single` report, after는 현재 Core 0.17.0이다. Core artifact·시각이 다르고 C/C++ runner 정책도 다르므로 변화율을 library 개선율로 주장하지 않는다. 단위 ops/s; report의 0.01 Kops/s 반올림값 기준.

| Pattern | Transport | Size B | C before | C++ before | C++ after | vs C++ before | after/C |
|---|---|---:|---:|---:|---:|---:|---:|
| DEALER_ROUTER_REQREP | tcp | 64 | 391,340 | 8,450 | 8,630 | +2.1% | 2.21% |
| DEALER_ROUTER_REQREP | tcp | 256 | 395,020 | 8,280 | 8,760 | +5.8% | 2.22% |
| DEALER_ROUTER_REQREP | tcp | 1024 | 398,050 | 8,220 | 8,570 | +4.3% | 2.15% |
| DEALER_ROUTER_REQREP | tcp | 65536 | 7,680 | 6,380 | 6,270 | -1.7% | 81.64% |
| DEALER_ROUTER_REQREP | tcp | 131072 | 6,630 | 5,150 | 4,890 | -5.0% | 73.76% |
| DEALER_ROUTER_REQREP | tcp | 262144 | 4,690 | 4,250 | 3,600 | -15.3% | 76.76% |
| DEALER_ROUTER_REQREP | ws | 64 | 372,850 | 8,160 | 7,070 | -13.4% | 1.90% |
| DEALER_ROUTER_REQREP | ws | 256 | 198,450 | 8,050 | 6,910 | -14.2% | 3.48% |
| DEALER_ROUTER_REQREP | ws | 1024 | 283,830 | 7,830 | 7,130 | -8.9% | 2.51% |
| DEALER_ROUTER_REQREP | ws | 65536 | 6,370 | 5,130 | 4,700 | -8.4% | 73.78% |
| DEALER_ROUTER_REQREP | ws | 131072 | 5,180 | 4,380 | 3,580 | -18.3% | 69.11% |
| DEALER_ROUTER_REQREP | ws | 262144 | 3,590 | 3,260 | 2,830 | -13.2% | 78.83% |
| DEALER_ROUTER_REQREP | ipc | 64 | 383,370 | 9,030 | 7,750 | -14.2% | 2.02% |
| DEALER_ROUTER_REQREP | ipc | 256 | 387,650 | 8,630 | 7,610 | -11.8% | 1.96% |
| DEALER_ROUTER_REQREP | ipc | 1024 | 407,540 | 8,580 | 7,410 | -13.6% | 1.82% |
| DEALER_ROUTER_REQREP | ipc | 65536 | 8,770 | 6,500 | 5,560 | -14.5% | 63.40% |
| DEALER_ROUTER_REQREP | ipc | 131072 | 7,250 | 5,650 | 4,720 | -16.5% | 65.10% |
| DEALER_ROUTER_REQREP | ipc | 262144 | 5,130 | 4,360 | 3,700 | -15.1% | 72.12% |
| ROUTER_ROUTER_REQREP | tcp | 64 | 443,000 | 8,140 | 7,120 | -12.5% | 1.61% |
| ROUTER_ROUTER_REQREP | tcp | 256 | 445,100 | 8,220 | 7,630 | -7.2% | 1.71% |
| ROUTER_ROUTER_REQREP | tcp | 1024 | 382,420 | 7,980 | 8,510 | +6.6% | 2.23% |
| ROUTER_ROUTER_REQREP | tcp | 65536 | 7,810 | 6,440 | 6,670 | +3.6% | 85.40% |
| ROUTER_ROUTER_REQREP | tcp | 131072 | 6,330 | 5,520 | 5,600 | +1.4% | 88.47% |
| ROUTER_ROUTER_REQREP | tcp | 262144 | 5,000 | 4,190 | 4,280 | +2.1% | 85.60% |
| ROUTER_ROUTER_REQREP | ws | 64 | 375,980 | 7,510 | 7,960 | +6.0% | 2.12% |
| ROUTER_ROUTER_REQREP | ws | 256 | 213,250 | 7,780 | 8,210 | +5.5% | 3.85% |
| ROUTER_ROUTER_REQREP | ws | 1024 | 186,850 | 7,890 | 7,180 | -9.0% | 3.84% |
| ROUTER_ROUTER_REQREP | ws | 65536 | 6,740 | 5,550 | 5,430 | -2.2% | 80.56% |
| ROUTER_ROUTER_REQREP | ws | 131072 | 5,290 | 4,450 | 4,450 | +0.0% | 84.12% |
| ROUTER_ROUTER_REQREP | ws | 262144 | 3,570 | 3,420 | 3,540 | +3.5% | 99.16% |
| ROUTER_ROUTER_REQREP | ipc | 64 | 437,990 | 8,310 | 8,970 | +7.9% | 2.05% |
| ROUTER_ROUTER_REQREP | ipc | 256 | 435,700 | 8,430 | 9,040 | +7.2% | 2.07% |
| ROUTER_ROUTER_REQREP | ipc | 1024 | 442,450 | 8,630 | 8,970 | +3.9% | 2.03% |
| ROUTER_ROUTER_REQREP | ipc | 65536 | 8,520 | 6,770 | 6,810 | +0.6% | 79.93% |
| ROUTER_ROUTER_REQREP | ipc | 131072 | 7,380 | 5,690 | 6,070 | +6.7% | 82.25% |
| ROUTER_ROUTER_REQREP | ipc | 262144 | 5,200 | 4,450 | 4,600 | +3.4% | 88.46% |

| Pattern | Transport | 평균 after/C | 평균 after/before |
|---|---|---:|---:|
| DEALER_ROUTER_REQREP | tcp | 39.79% | 98.35% |
| DEALER_ROUTER_REQREP | ws | 38.27% | 87.28% |
| DEALER_ROUTER_REQREP | ipc | 34.40% | 85.72% |
| ROUTER_ROUTER_REQREP | tcp | 44.17% | 99.02% |
| ROUTER_ROUTER_REQREP | ws | 45.61% | 100.64% |
| ROUTER_ROUTER_REQREP | ipc | 42.80% | 104.96% |

### Report 경로

- /home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260905_064250_p1cpp-single.txt
- /home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260905_070313_p1cpp-single.txt
- /home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_064712_p1cpp-single.txt
- /home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_070735_p1cpp-single.txt
- /home/hep7hep7/project/zlink-wt-cpp-single-reqrep/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_210500_cpp-single-reqrep-pass1-complete-nogo.txt

## Gate

- 공식 전체 `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=3 bash bindings/cpp/tests/run_tests.sh`: **PASS: contract 16/16, sample 7/7**. exit 0.
- 관련 contract 5종 각각 5회(`request_reply`, `request_writable_retry`, `exact_request_target`, `message`, `optimization_guard`): exit 0.
- `git diff --check`: exit 0. 공개 헤더·추적 소스 diff 0.

## BLOCKERS

- 순수 binding 성능 개선의 후보 없음. C와 C++의 serial/포화 제출 차이를 그대로 둔 상태에서 after/C를 library 효율로 판정할 수 없다.
- 오전 before와 현재 after는 동일 Core artifact의 paired A/B가 아니다. after 값은 현 상태 재측정으로만 제공한다.
- 최종 측정·gate의 남은 실패 없음. after 36/36 complete, RESULT 180/180, fail 0. 최대 load1=1.44, 외부 benchmark 겹침 0. 초기 중단 측정은 최종 판정에서 제외했다.

## 산출물

- 진행 로그: `/home/hep7hep7/project/zlink-work/c016/cpp-perf-single-reqrep-pass1-progress.md`
- 최종 콘솔: `/home/hep7hep7/project/zlink-work/c016/cpp-single-after-complete.log`
- Gate: `/home/hep7hep7/project/zlink-work/c016/cpp-single-gate.log`, `cpp-single-contract-repeat.log`
- 재현용 진단 도구: `bindings/cpp/build/trace_reqrep.c`, `finish_pass.py`, `analyze_release.py`, `analyze_timeline.py`(모두 ignored build 산출물).
- 프로파일·시간 계측 원본: `/home/hep7hep7/project/zlink-work/c016/cpp-single-*-release*`
- interposer는 build 영역의 진단 파일로만 생성했고 benchmark/gate에 LD_PRELOAD를 설정하지 않았다. 생산 소스에 임시 logging 없음.
