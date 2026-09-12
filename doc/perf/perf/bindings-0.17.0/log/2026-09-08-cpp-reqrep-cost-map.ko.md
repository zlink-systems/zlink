# C++ Multi REQREP 요청 비용의 함수별 귀속 지도

감독자가 C/C++ 요청당 비용의 위치를 판단하기 위한 측정 기록이다. 후보 제안·구현·채택/기각 판정은 없다.

**64 B DR의 C++ 잔여 비용 13,615.25 Ir/시도를 문맥별 self로 100% 귀속했다.**
기존 pass 1의 13,637.35 Ir/시도와 새 측정의 차이는 -22.10 Ir/시도다.
같은 새 측정에서 C는 6,499.90 Ir/시도이며, C++와의 차이는 7,115.35 Ir/시도다.
C의 현재 바이너리에서는 window가 상위 함수에 인라인되어 있으므로 이 C 값의 경계는 기존 6,447.38과 다르다. 아래에 차이를 명시했다.

**65536 B의 재시도 0회 지도는 확보하지 못했다.** 고정 조건에서 C는 DR/RR 모두 완료했지만 재시도가 있었고,
C++는 DR/RR 모두 `CLIENT_DONE` 없이 종료됐다. 65536 B 표는 이 관측을 숨기지 않은 실패·재시도 진단 표다.
성공·재시도 0회 조건을 충족한 64 B와 같은 모집단으로 해석하지 않는다.

## 집계 경계와 검산 방법

- 분모 `N = zlink_request_part calls / 2`. 첫 payload part와 빈 마지막 part가 한 REQUEST 시도다.
  완료 수, throughput × duration, `capture` 횟수를 분모로 쓰지 않았다.
- C++ window는 `client_bench_t<SocketT>::run()` 전체다. 기존 pass 1과 동일하게 setup·active loop·drain·통계 출력을 포함한다.
- 현재 C 바이너리는 `run_measurement_window()`가 `run_client_benchmark()`에 인라인되어 별도 함수로 남아 있지 않다.
  새 C 표는 **`run_client_benchmark()` 전체**이며 setup·종료 처리를 포함한다. DR setup의 분리된 비용은
  76.39 Ir/시도이고, 상위 함수에 인라인된 setup/종료 명령은 runner 행에 남는다.
  기존 C window의 6,447.38을 새 C 값으로 덮어쓰거나 두 경계를 동일하다고 간주하지 않는다.
- window 안에서 `zlink_request_part`와 그 모든 하위 문맥의 self를 제외한다. 다른 Core 호출인 poller wait,
  completion recv/close, message copy/close는 제외하지 않는다.
- `--separate-callers=40`으로 공유 allocator·mutex까지 호출 문맥을 구분했다. client의 최대 관측 호출 문맥 길이는
  36이므로 40에서 잘린 문맥은 없었다.
  모든 raw function ID의 self를 정확히 한 행에만 넣는다. 비용 비율에 따른 추정 배분은 없다.
- 표의 **귀속 self**는 해당 행에 속한 함수·하위 함수의 self 합이다. 예를 들어 malloc 내부 self도 그 할당을 발생시킨
  행에 한 번만 포함된다. 실제 함수별 self와 문맥별 raw ID는 `*.functions.tsv`, `*.contexts.tsv`에 전수 보존했다.
- ¹ inclusive는 표시한 대표 함수의 참고 값이다. 그 함수가 호출하는 다른 행과 native REQUEST를 포함할 수 있다.
  **inclusive 열은 합산하지 않는다.** 합계는 self 열만 합산한다.
- ² 주표 new는 해당 window에서 native REQUEST를 제외한 문맥의 호출 수다. 기존 **6.407509는 프로세스 전체** 값이다.
  이 둘은 범위가 달라 같은 합계 칸에 놓을 수 없다. malloc 호출은 new와 중첩되므로 더하지 않았다.
- Release 바이너리의 인라인 명령을 별도 가상 함수로 나누지 않았다. actor 안의 인라인 vector 이동은 actor self에,
  capture/settle 안의 vector 이동은 capture/settle self에 남는다. 따라서 “reply vector의 분리된 호출”은 vector 전체 비용을 뜻하지 않는다.
  frame 행도 actor와 분리된 할당/해제 비용이다. `run`에 인라인된 frame 초기화는 runner 자체 self에 남는다.
- 표시 값은 소수 둘째 자리에서 반올림했다. 아래 원시 정수 검산이 정확한 합계를 소유한다.

| 원본 pass 1 DR 64 B 경계 | C | C++ |
|---|---:|---:|
| N | 20,052 | 10,920 |
| window Ir/시도 | 12,790.45 | 20,113.95 |
| native REQUEST Ir/시도 | 6,343.07 | 6,476.61 |
| 잔여 Ir/시도 | **6,447.38** | **13,637.35** |

## 1. 64 B DR — C++와 C

### C++: 13,615.25 Ir/시도

| 항목 | 함수/경로 | 귀속 self Ir/시도 | 대표 함수 inclusive Ir/시도¹ | new/시도² | 비중 |
|---|---|---:|---:|---:|---:|
| coroutine actor·frame 수명 | `submit_async_request [actor]` + `run → new` + actor의 unsized delete | 516.06 | 10,445.01 | 1.00012 | 3.79% |
| REQUEST bundle 생성·소멸 | `request_submit_operation_t::async` + entry 생성자 + bundle `_M_dispose/_M_destroy` | 528.78 | 8,164.56 | 1.00000 | 3.88% |
| request builder·operation 준비 | `request/message/timeout`, actor → `message_t::from` 및 message 수명 | 753.93 | — | 0.00059 | 5.54% |
| submit_raw_request_state·part snapshot | `submit_raw_request_state`의 native REQUEST 밖 part 준비·copy·close | 714.01 | 7,216.82 | 0.00000 | 5.24% |
| entry submit·publication | `start_request/submit_request_attempt/retry`의 제출 상태·publication | 333.01 | 7,542.83 | 0.00012 | 2.45% |
| completion entry 등록·해제 | `completion_owner_t::register_entry/unregister_entry`와 map/PMR | 614.35 | — | 0.00000 | 4.51% |
| async await·result 소비 | `await_suspend`, result의 `suspend/ready/take/abandon` | 527.00 | 407.94 | 0.00000 | 3.87% |
| scheduler closure·ready queue | scheduler 등록 lambda, `resume_async_slot`, closure manager, `run_ready_round` | 825.56 | 339.59 | 2.06303 | 6.06% |
| reply vector의 분리된 호출 | capture → vector 할당·message 생성/adopt 보조 호출 + actor의 sized delete | 452.55 | — | 1.00000 | 3.32% |
| capture·settle 합류 | `capture/settle_if_joined`의 self·lock·notification | 675.00 | 1,810.97 | 0.00000 | 4.96% |
| binding completion drain | `completion_owner_t::drain`의 self·lock 등 | 381.52 | 3,119.63 | 0.00000 | 2.80% |
| binding poller wrapper | `poller_t::impl::wait` wrapper·lock 등 | 75.63 | 8,713.49 | 0.00012 | 0.56% |
| Core poller wait·reply 진행 | `zlink_poller_wait`와 Core 하위 함수·PLT | 5,516.24 | 5,518.36 | 0.00012 | 40.52% |
| Core completion_recv | `zlink_completion_recv`와 하위 함수·PLT | 626.84 | 623.90 | 0.00000 | 4.60% |
| Core completion_close | `zlink_completion_close`와 하위 함수·PLT | 309.93 | 307.93 | 0.00024 | 2.28% |
| reply 검증·latency 표본 | `observe_reply`와 검증·timestamp·latency 표본 | 399.67 | 399.67 | 0.00000 | 2.94% |
| setup | C++ `setup` / C `setup_client_state`의 전체 self | 124.25 | — | 0.07312 | 0.91% |
| runner loop·clock·통계 및 기타 | runner 자체 self, clock, 통계 정렬·출력, 나머지 함수 | 240.90 | — | 0.00000 | 1.77% |
| **합계** | **문맥별 self의 서로 겹치지 않는 합** | **13,615.25** | **합산 안 함** | **5.13746** | **100.00%** |

### C: 6,499.90 Ir/시도

| 항목 | 함수/경로 | 귀속 self Ir/시도 | 대표 함수 inclusive Ir/시도¹ | new/시도² | 비중 |
|---|---|---:|---:|---:|---:|
| Core poller wait·reply 진행 | `zlink_poller_wait`와 Core 하위 함수·PLT | 4,593.91 | 4,593.41 | 0.00007 | 70.68% |
| Core completion_recv | `zlink_completion_recv`와 하위 함수·PLT | 515.34 | 512.98 | 0.00000 | 7.93% |
| Core completion_close | `zlink_completion_close`와 하위 함수·PLT | 368.63 | 366.63 | 0.00013 | 5.67% |
| C message 준비·소비 | runner → `zlink_msg_*` 및 payload `memcpy` | 394.79 | — | 0.00000 | 6.07% |
| C completion 처리 | `drain_socket_completions`의 나머지 self | 0.56 | — | 0.00000 | 0.01% |
| setup | C++ `setup` / C `setup_client_state`의 전체 self | 76.39 | — | 0.03888 | 1.18% |
| runner loop·clock·통계 및 기타 | runner 자체 self, clock, 통계 정렬·출력, 나머지 함수 | 550.27 | — | 0.00321 | 8.47% |
| **합계** | **문맥별 self의 서로 겹치지 않는 합** | **6,499.90** | **합산 안 함** | **0.04229** | **100.00%** |

C의 `record_request_completion`, submit loop 등의 인라인 명령은 runner 자체 self에 포함된다.
C++에서 별도 함수로 남은 작업과 C의 인라인 작업을 함수명만으로 1:1 동일시하지 않는다.

### 대표 함수 자체의 self와 포함 관계

| 함수 | 함수 자체 self Ir/시도 | inclusive Ir/시도 | 주표에서 갈라지는 하위 비용 |
|---|---:|---:|---|
| `coroutine actor` | 261.00 | 10,445.01 | async 제출, await, observe, vector/frame 해제 |
| `request_submit_operation_t::async` | 183.00 | 8,164.56 | bundle, registry, 제출 |
| `submit_raw_request_state` | 283.00 | 7,216.82 | snapshot + 제외한 native REQUEST |
| `submit_request_attempt` | 153.00 | 7,542.83 | publication + snapshot + native REQUEST |
| `capture` | 204.00 | 1,810.97 | 합류 + vector 보조 호출 + completion close + scheduler |
| `settle_if_joined` | 178.00 | 814.40 | 합류 + scheduler |
| `completion_owner_t::drain` | 187.27 | 3,119.63 | drain + recv + capture + registry |
| `await_suspend` | 153.00 | 407.94 | await + scheduler 등록 closure |
| `resume_async_slot` | 92.00 | 339.59 | 재개 closure + queue 등록 |

기존 capture inclusive 1,809.81과 새 값 1810.97는 같은 포함 경계다.
주표 capture·settle의 배타적 귀속은 **675.00 Ir/시도**다. inclusive 전체를 다시 더하면
vector·scheduler·completion close를 중복 계상한다.

### Core poller의 함수별 self

잔여 비용 중 Core poller는 C++ **5,516.24 Ir/시도(40.52%)**,
C **4,593.91 Ir/시도(70.68%)**다. 양쪽 차이는 **922.32 Ir/시도**다.
`zlink_request_part`만 제외한 구간에는 이 Core 실행이 남아 있다. 아래 표도 서로 다른 함수의 self만 합산한다.

| Core poller 하위 함수 — 이 문맥에서의 self만 | C++ Ir/시도 | C Ir/시도 |
|---|---:|---:|
| `pthread_mutex_lock@@GLIBC_2.2.5` | 711.94 | 551.38 |
| `pthread_mutex_unlock@@GLIBC_2.2.5` | 562.28 | 432.70 |
| `zlink::socket_base_t::process_commands(int, bool, bool, unsigned long const*, bool) [clone .constprop.1]` | 408.72 | 330.02 |
| `zlink::socket_reqrep_internal::(anonymous namespace)::complete_reply_from_transport(std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> const&, unsigned long, unsigned long, zlink::pipe_t*, unsigned long, unsigned char, unsigned long, zlink_msg_t*, unsigned long)` | 303.41 | 296.32 |
| `zlink::mailbox_t::recv(zlink::command_t*, int, bool)` | 242.40 | 167.78 |
| `zlink::socket_reqrep_internal::process_completion_pipe(zlink::socket_base_t*, zlink::pipe_t*)` | 170.62 | 152.75 |
| `zlink::msg_t::move(zlink::msg_t&)` | 152.00 | 149.71 |
| `zlink::msg_t::close()` | 150.00 | 148.86 |
| `zlink::socket_base_t::process_ready_completion_pipes()` | 136.75 | 112.59 |
| `zlink::pipe_t::account_inbound_frame(zlink::msg_t const*, bool)` | 112.93 | 111.10 |
| `zlink::socket_base_t::get_events_internal(int, unsigned int*, bool)` | 109.17 | 113.17 |
| `zlink::ypipe_t<zlink::msg_t, 64>::read(zlink::msg_t*, bool*)` | 100.25 | 100.25 |
| `zlink::msg_t::init()` | 96.00 | 94.55 |
| `zlink::pipe_t::read(zlink::msg_t*)` | 96.00 | 96.00 |
| `zlink::socket_lifecycle_coordinator_t::lock_public_api_sync()` | 86.57 | 69.35 |
| 나머지 함수 전수 TSV | 2,077.18 | 1,667.40 |
| **Core poller self 합계** | **5,516.24** | **4,593.91** |

## 2. 원본 operator new 6.407509회의 호출자별 지도

pass 1 원본 C++ N=10,920, C N=20,052를 사용했다. PLT처럼 주소만 남은 전달 함수를 넘어 실제 호출자를 추적했다.
원시 호출 수의 합은 각각 69,970과 12,557로 일치한다.

| 실제 new 호출자 / 할당 | C++ 호출 수 | C++ new/시도 | C++ 할당 호출 경계 Ir/시도³ | C 호출 수 | C new/시도 |
|---|---:|---:|---:|---:|---:|
| coroutine frame 및 snapshot — `run` | 10,921 | 1.000092 | 95.04 | 0 | 0.000000 |
| reply vector — `capture` | 10,920 | 1.000000 | 81.82 | 0 | 0.000000 |
| REQUEST bundle — `async` | 10,920 | 1.000000 | 83.39 | 0 | 0.000000 |
| scheduler 등록 closure — `await_suspend` | 10,920 | 1.000000 | 71.98 | 0 | 0.000000 |
| 재개 closure — `resume_async_slot` | 10,920 | 1.000000 | 75.70 | 0 | 0.000000 |
| Core `start_async_write` | 7,761 | 0.710714 | 46.31 | 6,581 | 0.328197 |
| Core `start_async_read` | 5,498 | 0.503480 | 32.80 | 3,526 | 0.175843 |
| ready queue deque block — `_M_push_back_aux` | 682 | 0.062454 | 4.06 | 0 | 0.000000 |
| `basic_string::_M_create` | 372 | 0.034066 | 4.01 | 397 | 0.019799 |
| nothrow new → throwing new | 158 | 0.014469 | 2.88 | 1,243 | 0.061989 |
| 그 밖 모든 호출자 — 전수 TSV에 개별 기재 | 898 | 0.082234 | 14.39 | 810 | 0.040395 |
| **프로세스 전체 합계** | **69,970** | **6.407509** | 참고 경계 비용 | **12,557** | **0.626222** |

³ 할당 호출 경계 Ir는 해당 caller → new PLT 경계 아래에서 실행된 비용이다. malloc 내부를 포함하며
주표 비용과 겹친다. 호출자 자신의 할당 준비 명령은 포함하지 않는다. 이를 주표 self에 추가하지 않는다.

- `run`의 10,921회는 요청 frame 10,920회와 최종 통계 snapshot 1회를 함께 포함한다.
- `await_suspend`의 요청당 1회는 **scheduler 등록 lambda의 std::function 저장 공간**이다.
  `perf_socket_adapter.hpp:93`의 lambda는 shared_ptr를 캡처한다. 바이너리에서 해당 new 인자는 16 B이고
  뒤에서 `continuation_scheduler`의 `_M_invoke/_M_manager`를 설정한다. `alloc-disassembly.txt`에 보존했다.
- `resume_async_slot`의 요청당 1회는 scheduled slot을 캡처하는 **재개 lambda의 std::function 저장 공간**이다.
- 별도 resume-slot allocation을 이 두 회에 중복 계산하지 않았다. `async_operation_state.hpp:122-126`의
  bundle lifetime alias 경로를 사용하며, 이 측정의 `suspend` 하위 new 호출은 **0회**다.
- 요청당 다섯 주요 new를 제외한 C++ **1.407509회/시도** 중 Core async write/read가
  **1.214194회/시도**, deque block 확장이 **0.062454회/시도**다. setup·I/O thread 비용을 포함하는 수치다.

전수 파일: C++ 호출자 TSV (`/tmp/zlink-cpp-reqrep-cost-map/cpp-original-new-callers.tsv`),
C 호출자 TSV (`/tmp/zlink-cpp-reqrep-cost-map/c-original-new-callers.tsv`).
주표와 동일한 새 셀의 전체 new/시도는 C++ 6.464981, C 0.807523다.
원본 6.407509와 새 값을 섞어서 합계를 만들지 않았다.

## 3. 65536 B — 실패·재시도 셀의 비용 분포

조건은 64 B와 동일한 clients 4, duration 2 s, 양쪽 callgrind다. C++ DR은 native 시도 35,939,
새 async 820, 재시도 35,119회였고 RR은 native 시도 33,380, 새 async 780, 재시도 32,600회였다.
두 C++ 셀은 완료하지 못했다. C DR은 시도 2,044 / 정상 reply 1,024 / 재시도 1,020,
C RR은 시도 1,876 / 정상 reply 940 / 재시도 936으로 완료했다.

아래 %는 **해당 관측 셀의 window − native REQUEST** 안에서의 비중이며, 정상 요청 수명 전체의 비중이 아니다.
특히 C++의 분모에는 동일 operation의 재제출이 반복되어 frame·bundle의 요청당 1회 생성 비용이 희석된다.
64 B의 성공·재시도 0회 셀과 비교한 pp는 관측값만 표시한다.

### DR C++

| 항목 | 64 B self Ir/시도 | 64 B 비중 | 65536 B self Ir/시도 | 65536 B 비중 | 비중 변화 | 65536 B new/시도 |
|---|---:|---:|---:|---:|---:|---:|
| coroutine actor·frame 수명 | 516.06 | 3.79% | 18.03 | 0.40% | -3.39 pp | 0.02282 |
| REQUEST bundle 생성·소멸 | 528.78 | 3.88% | 11.92 | 0.27% | -3.62 pp | 0.02282 |
| request builder·operation 준비 | 753.93 | 5.54% | 1,542.76 | 34.65% | +29.11 pp | 0.07624 |
| submit_raw_request_state·part snapshot | 714.01 | 5.24% | 591.43 | 13.28% | +8.04 pp | 0.00000 |
| entry submit·publication | 333.01 | 2.45% | 258.24 | 5.80% | +3.35 pp | 0.00011 |
| completion entry 등록·해제 | 614.35 | 4.51% | 14.86 | 0.33% | -4.18 pp | 0.00000 |
| async await·result 소비 | 527.00 | 3.87% | 294.81 | 6.62% | +2.75 pp | 0.00000 |
| scheduler closure·ready queue | 825.56 | 6.06% | 18.10 | 0.41% | -5.66 pp | 0.04719 |
| reply vector의 분리된 호출 | 452.55 | 3.32% | 8.34 | 0.19% | -3.14 pp | 0.01130 |
| capture·settle 합류 | 675.00 | 4.96% | 137.59 | 3.09% | -1.87 pp | 0.00000 |
| binding completion drain | 381.52 | 2.80% | 481.13 | 10.81% | +8.00 pp | 0.08509 |
| binding poller wrapper | 75.63 | 0.56% | 2.49 | 0.06% | -0.50 pp | 0.00003 |
| Core poller wait·reply 진행 | 5,516.24 | 40.52% | 421.39 | 9.46% | -31.05 pp | 0.00003 |
| Core completion_recv | 626.84 | 4.60% | 448.69 | 10.08% | +5.47 pp | 0.00000 |
| Core completion_close | 309.93 | 2.28% | 64.99 | 1.46% | -0.82 pp | 0.00000 |
| reply 검증·latency 표본 | 399.67 | 2.94% | 3.90 | 0.09% | -2.85 pp | 0.00000 |
| setup | 124.25 | 0.91% | 36.15 | 0.81% | -0.10 pp | 0.01711 |
| runner loop·clock·통계 및 기타 | 240.90 | 1.77% | 97.64 | 2.19% | +0.42 pp | 0.06912 |
| **합계** | **13,615.25** | **100.00%** | **4,452.45** | **100.00%** | **0.00 pp** | **0.35185** |

### DR C

| 항목 | 64 B self Ir/시도 | 64 B 비중 | 65536 B self Ir/시도 | 65536 B 비중 | 비중 변화 | 65536 B new/시도 |
|---|---:|---:|---:|---:|---:|---:|
| Core poller wait·reply 진행 | 4,593.91 | 70.68% | 8,582.13 | 11.17% | -59.51 pp | 0.00049 |
| Core completion_recv | 515.34 | 7.93% | 621.84 | 0.81% | -7.12 pp | 0.00000 |
| Core completion_close | 368.63 | 5.67% | 300.45 | 0.39% | -5.28 pp | 0.00000 |
| C message 준비·소비 | 394.79 | 6.07% | 65,938.63 | 85.81% | +79.74 pp | 0.00000 |
| C completion 처리 | 0.56 | 0.01% | 0.63 | 0.00% | -0.01 pp | 0.00000 |
| setup | 76.39 | 1.18% | 560.22 | 0.73% | -0.45 pp | 0.28474 |
| runner loop·clock·통계 및 기타 | 550.27 | 8.47% | 837.20 | 1.09% | -7.38 pp | 0.02348 |
| **합계** | **6,499.90** | **100.00%** | **76,841.09** | **100.00%** | **0.00 pp** | **0.30871** |

DR 65536 B C의 payload 준비 행에서 `memcpy` 계열 자체 self는 **65,474.46 Ir/시도**다.
DR 65536 B C++의 payload 준비 행에서 `memcpy` 계열 자체 self는 **1,495.51 Ir/시도**다.

이 자료로 기존 latency 2.7x의 원인별 기여율을 확정할 수는 없다. 완료하지 못한 C++ 셀과 재시도 분모의 비중을
성공 요청의 시간 비중으로 바꾸지 않았다. 요청한 **65536 B 성공·재시도 0회 비교는 미완료**다.

## 4. RR 요약

64 B RR의 잔여 비용은 C++ **14,127.74**, C **6,705.14 Ir/시도다.
DR과 동일한 함수 분류로 합계가 맞으며 두 64 B 셀의 재시도는 0회다. 65536 B에는 앞 절의 실패·재시도 제한이 그대로 적용된다.

| 항목 | RR 64 B C++ self | RR 64 B C self | RR 65536 B C++ self (실패) | RR 65536 B C self (재시도 있음) |
|---|---:|---:|---:|---:|
| coroutine actor·frame 수명 | 523.96 | 0.00 | 17.91 | 0.00 |
| REQUEST bundle 생성·소멸 | 531.46 | 0.00 | 12.55 | 0.00 |
| request builder·operation 준비 | 886.64 | 0.00 | 1,581.71 | 0.00 |
| submit_raw_request_state·part snapshot | 719.00 | 0.00 | 596.66 | 0.00 |
| entry submit·publication | 333.01 | 0.00 | 258.63 | 0.00 |
| completion entry 등록·해제 | 608.42 | 0.00 | 15.23 | 0.00 |
| async await·result 소비 | 527.00 | 0.00 | 263.66 | 0.00 |
| scheduler closure·ready queue | 826.46 | 0.00 | 20.58 | 0.00 |
| reply vector의 분리된 호출 | 455.14 | 0.00 | 9.72 | 0.00 |
| capture·settle 합류 | 675.00 | 0.00 | 138.79 | 0.00 |
| binding completion drain | 380.03 | 0.00 | 472.04 | 0.00 |
| binding poller wrapper | 75.56 | 0.00 | 2.56 | 0.00 |
| Core poller wait·reply 진행 | 5,872.60 | 4,776.20 | 457.77 | 8,422.18 |
| Core completion_recv | 627.61 | 520.87 | 451.94 | 624.87 |
| Core completion_close | 311.43 | 376.00 | 65.52 | 296.99 |
| reply 검증·latency 표본 | 399.65 | 0.00 | 4.55 | 0.00 |
| C message 준비·소비 | 0.00 | 397.54 | 0.00 | 65,857.43 |
| C completion 처리 | 0.00 | 0.46 | 0.00 | 1.08 |
| setup | 131.64 | 78.04 | 39.38 | 616.37 |
| runner loop·clock·통계 및 기타 | 243.12 | 556.04 | 86.87 | 944.10 |
| **합계** | **14,127.74** | **6,705.14** | **4,496.09** | **76,763.01** |

RR 64 B Core poller 비중은 C++ 41.57%, C 71.23%다.
RR 65536 B 전체 new/시도는 C++ 2.466537, C 5.498934이며
서로 다른 성공 상태의 관측값이다. 함수별·문맥별 파일에는 RR도 DR과 같은 상세 항목을 전수 보존했다.

## 5. C에 같은 함수가 없는 C++ 경로

아래는 C에 동일한 binding/coroutine 함수가 없는 경로의 **배타적 self 귀속**이다.
C에도 메시지 준비·결과 처리 등의 인라인 작업은 존재하므로, 이 소계를 곧바로 순수 언어 격차로 간주하지 않는다.

| C++ 경로 | DR 64 B Ir/시도 | RR 64 B Ir/시도 |
|---|---:|---:|
| coroutine actor·frame 수명 | 516.06 | 523.96 |
| REQUEST bundle 생성·소멸 | 528.78 | 531.46 |
| request builder·operation 준비 | 753.93 | 886.64 |
| submit_raw_request_state·part snapshot | 714.01 | 719.00 |
| entry submit·publication | 333.01 | 333.01 |
| completion entry 등록·해제 | 614.35 | 608.42 |
| async await·result 소비 | 527.00 | 527.00 |
| scheduler closure·ready queue | 825.56 | 826.46 |
| reply vector의 분리된 호출 | 452.55 | 455.14 |
| capture·settle 합류 | 675.00 | 675.00 |
| binding completion drain | 381.52 | 380.03 |
| binding poller wrapper | 75.63 | 75.56 |
| **해당 함수 경로 소계** | **6,397.42** | **6,541.69** |

| DR 64 B 잔여 격차의 검산 | C++ − C Ir/시도 |
|---|---:|
| 위 C++ 함수 경로 소계 | +6397.42 |
| Core poller wait·reply 진행 | +922.32 |
| Core completion_recv | +111.50 |
| Core completion_close | -58.70 |
| reply 검증·latency 표본 | +399.67 |
| C message 준비·소비 | -394.79 |
| C completion 처리 | -0.56 |
| setup | +47.86 |
| runner loop·clock·통계 및 기타 | -309.37 |
| **격차 합계** | **+7115.35** |

DR의 전체 잔여 격차 7,115.35 Ir/시도는 이 소계 외에
Core poller 차이 922.32, completion recv 차이 111.50,
completion close 차이 -58.70, C의 message/runner 처리 및 setup 경계 차이를 함께 포함한다.

소스 대응 위치:

- [coroutine 생성·재개·reply 관찰](../../../../../bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp): `run:216`, `observe_reply:425`, `submit_async_request:452`.
- [REQUEST bundle](../../../../../bindings/cpp/src/Runtime/Messaging/request_reply.cpp): `request_completion_bundle_t:16`, `async:100`, `make_shared:114`.
- [completion entry/owner](../../../../../bindings/cpp/src/Runtime/Messaging/completion_owner.cpp): `submit_request_attempt:212`, `capture:298`, `settle_if_joined:451`, `register_entry:499`, `drain:572`.
- [async result/continuation](../../../../../bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp): `resume_async_slot:50`, `suspend:113`, lifetime alias `:122`.
- [public awaiter](../../../../../bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp): `await_suspend:83`.
- [scheduler queue](../../../../../bindings/cpp/perf/common/perf_socket_adapter.hpp): `continuation_scheduler:93`, `run_ready_round:110`.
- [C 비교 경로](../../../../../bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp): `submit_request:218`, `drain_socket_completions:352`, `run_measurement_window:527`.

## 6. 측정 조건·원시 report·정규화 분모

```text
branch: main
Core source: release
Core package: /home/hep7/.cache/zlink/core-pinned/0.17.1
LD_LIBRARY_PATH: /home/hep7/.cache/zlink/core-pinned/0.17.1/lib
transport: tcp
patterns: MULTI_DEALER_ROUTER_REQREP, MULTI_ROUTER_ROUTER_REQREP
sizes: 64, 65536
PERF_MULTI_CLIENTS=4
PERF_MULTI_DURATION_SECONDS=2
PERF_DURATION_SECONDS=2
PERF_IO_THREADS=4
PERF_CONNECT_CONCURRENCY=128
REQUEST timeout: default 200 ms (변경 없음)
callgrind: --tool=callgrind --cache-sim=no --collect-jumps=no
문맥별 self 지도 추가 옵션: --separate-callers=40
client 및 server 모두 callgrind, 매 셀 직렬 실행
```

실행 전마다 `bash scripts/perf/wait-for-idle-perf.sh`를 호출했다. 다른 perf가 실행 중이면 대기했고 load>5에서도 대기했다.
현재 C/C++ binary와 Core library를 재빌드하지 않았다. `binaries.sha256`과 `*-ldd.txt`에 실제 실행 파일·Core 경로를 보존했다.

| 셀 | N (native 시도) | 정상 reply 관측 | 재시도 | status | window Ir 원시 정수 | 제외 native Ir | 귀속 self 합계 Ir |
|---|---:|---:|---:|---|---:|---:|---:|
| C++ DR 64 | 8,424 | 8,424 | 0 | complete | 169,474,531 | 54,779,679 | **114,694,852** |
| C DR 64 | 14,968 | 14,968 | 0 | complete | 192,930,653 | 95,640,140 | **97,290,513** |
| C++ DR 65536 | 35,939 | 406 | 35,119 | 실패 | 476,478,459 | 316,462,008 | **160,016,451** |
| C DR 65536 | 2,044 | 1,024 | 1,020 | complete | 172,250,359 | 15,187,167 | **157,063,192** |
| C++ RR 64 | 8,052 | 8,052 | 0 | complete | 170,323,014 | 56,566,431 | **113,756,583** |
| C RR 64 | 14,808 | 14,808 | 0 | complete | 202,089,303 | 102,799,621 | **99,289,682** |
| C++ RR 65536 | 33,380 | 440 | 32,600 | 실패 | 459,367,000 | 309,287,578 | **150,079,422** |
| C RR 65536 | 1,876 | 940 | 936 | complete | 158,838,664 | 14,831,252 | **144,007,412** |

C++ 실패 셀의 정상 reply 수는 전체 async 결과의 성공률 분모가 아니다. C++ 재시도는 `completion_entry_t::retry()` 호출 수와
`submit_request_attempt − async`가 일치한다. C 완료 셀은 모든 outstanding/retained 요청을 drain한 뒤 종료했으며,
재시도 수는 native 시도 − `complete_reply_from_transport` 호출 수다. active 구간 출력 throughput은 final drain 수를 포함하지 않으므로 이 검산에 쓰지 않았다.

각 셀에서 다음 세 정수 등식이 모두 통과했다.

```text
sum(all function-context self Ir) == callgrind summary Ir
sum(window function-context self Ir) == window root inclusive Ir
sum(분해 표의 raw self Ir) == window Ir - native REQUEST subtree self Ir
```

실행·분석 파일은 `/tmp/zlink-cpp-reqrep-cost-map/`에 있다.

| 접두어 (`.client.cg`, `.server.cg`, `.json` 등을 붙인다) | 상태 |
|---|---|
| `cpp_cpp_dealer_router_reqrep_64_4_2_both` | client/server complete |
| `c_c_dealer_router_reqrep_64_4_2_both` | client/server complete |
| `cpp_cpp_dealer_router_reqrep_65536_4_2_both` | client 실패; 최초 server는 harness 종료로 profile 미완성 |
| `c_c_dealer_router_reqrep_65536_4_2_both` | client/server complete |
| `cpp_cpp_router_router_reqrep_64_4_2_both` | client/server complete |
| `c_c_router_router_reqrep_64_4_2_both` | client/server complete |
| `cpp_cpp_router_router_reqrep_65536_4_2_both` | client 실패; 최초 server는 harness 종료로 profile 미완성 |
| `c_c_router_router_reqrep_65536_4_2_both` | client/server complete |

- `*.client.cg`, `*.server.cg`: callgrind 원본. 실패한 C++ 최초 두 셀의 server 파일은 0 B이며 성공 report로 세지 않았다.
- `*.client.log`, `*.client.err`, `*.server.log`, `*.server.err`, `*.run.log`: benchmark 출력·callgrind 출력·실행 조건.
- `*.map.json`: raw self 합계, 분모, 분류, 참고 inclusive, new 호출 수.
- `*.functions.tsv`: 모든 함수의 **분류별 self**. 한 함수가 여러 문맥에 쓰이면 분류별 행으로 구분된다.
- `*.contexts.tsv`: 함수 ID, self, inclusive, calls, 전체 호출 문맥. 계산을 원본까지 역추적할 수 있다.
- `parse.py`, `map.py`, `alloc_original.py`, `pair.py`, `report.py`: 파서·집계·실행·기록 생성 script.
- `binaries.sha256`, `head.txt`, `c-ldd.txt`, `cpp-ldd.txt`, `alloc-disassembly.txt`: 실행 대상과 호출자 확인 근거.
- 원본 pass 1: `/tmp/zlink-cpp-reqrep-pass1/{c_c,cpp_cpp}_dealer_router_reqrep_64_4_2_both.client.cg`.

### 호출 문맥 분리 옵션을 뺀 65536 B 확인

추가 옵션의 영향을 구분하기 위해 기본 callgrind 옵션만으로 같은 65536 B 조건을 한 번씩 확인했다.
실패 시 server에 STOP을 보내 양쪽 종료 profile을 보존하도록 `/tmp` harness의 정리 절차만 보완했다.
request timeout·clients·duration은 바꾸지 않았다.

| 셀 | 결과 | report 접두어 |
|---|---|---|
| DR | fail: client ended (exit 1) | `cpp_cpp_dealer_router_reqrep_65536_4_2_both-base` |
| ↳ native 시도 / 재시도 | 42,772 / 41,856 | 같은 profile |
| RR | fail: client ended (exit 1) | `cpp_cpp_router_router_reqrep_65536_4_2_both-base` |
| ↳ native 시도 / 재시도 | 42,375 / 41,455 | 같은 profile |

기본 옵션 DR의 JSON `load=6.334...`는 **load 대기 시작 시점** 값이다. 실행 script가 load≤5가 될 때까지 기다린 뒤 실행했으며, 실행 순간의 수치는 따로 저장하지 않았다. 문맥 분리 8셀의 시작 load는 모두 5 이하였다.

### 새 셀의 new 범위별 검산

| 셀 | 잔여 window new/시도 | native REQUEST 안 | window 밖 | 프로세스 전체 |
|---|---:|---:|---:|---:|
| cpp_cpp_dealer_router_reqrep_64_4_2_both | 5.137464 | 0.003561 | 1.323955 | **6.464981** |
| c_c_dealer_router_reqrep_64_4_2_both | 0.042290 | 0.119254 | 0.645978 | **0.807523** |
| cpp_cpp_dealer_router_reqrep_65536_4_2_both | 0.351846 | 2.974985 | 0.091822 | **3.418654** |
| c_c_dealer_router_reqrep_65536_4_2_both | 0.308708 | 1.517123 | 3.386497 | **5.212329** |
| cpp_cpp_router_router_reqrep_64_4_2_both | 5.570417 | 0.072156 | 1.383010 | **7.025584** |
| c_c_router_router_reqrep_64_4_2_both | 0.156267 | 0.180511 | 0.695908 | **1.032685** |
| cpp_cpp_router_router_reqrep_65536_4_2_both | 0.369682 | 1.982355 | 0.114500 | **2.466537** |
| c_c_router_router_reqrep_65536_4_2_both | 0.853412 | 1.018657 | 3.626866 | **5.498934** |

모든 새 셀의 호출자별 수치는 `*.new-callers.tsv`에 전수 보존했다. 최초 두 C++ 실패 셀은 성공 JSON을 만들지 않았으므로 status는 `.run.log`의 `client ended`와 CLIENT_DONE 부재를 근거로 기록했다.

## 변경·검증 범위

저장소에서 새로 작성한 파일은 이 기록뿐이다. `bindings/**`, `core/**`, `framework/**` source 수정과
임시 source 계측은 **없다**. Core·binding 재빌드, 정책·스펙·계획서·decisions 수정, commit/push도 없다.
실행 시작 시 존재한 C perf 소스 변경과 untracked 문서·benchmark 로그는 직접 수정하지 않았다.
측정 중 외부 작업으로 HEAD가 `84250d4de11dd8cfe31849165c1ebb1f316f6b2c`에서
`1aa2751b1ba9bca04e77120d6c9a880e9173f6ca`로 바뀌었다. 이 job은 commit을 실행하지 않았으며,
실행 바이너리 8개와 고정 Core의 SHA-256은 시작 값과 끝 값이 모두 일치했다.

검증은 위 raw self/분모/new 호출 합계 검산과 문서 표·링크 검증이다. runtime을 수정하지 않아 기능 test/gate는 실행하지 않았다.
남은 요구는 **고정 조건의 65536 B 성공·재시도 0회 지도**다. 실패·재시도 셀을 그 대체 성공 결과로 보고하지 않는다.
