# C++ Single 수신 hot-path pass 1 결과

## 결론

`received_t` 수신 library 경로는 이미 caller-owned wrapper, vector capacity 재사용, native
storage 직접 수신, 정수형 EAGAIN 반환을 쓴다. callgrind에서 C++ `socket_t::receive` 비용은
5.58 kIr/message, C의 수신 helper에서 runner의 환경 변수 조회를 뺀 비용은 5.38
kIr/message였다. 공개 API, ownership, error contract를 그대로 두면서 5% 이상 줄일 library
후보는 없어서 library 변경은 no-go로 판정했다.

실제 큰 비용은 Single C++ runner가 고정 환경 변수 `PERF_PART_COUNT`를 메시지마다 다시
읽은 것이었다. 수신에서 메시지마다 두 번, 송신에서 한 번 `getenv`/`strcmp`를 실행했다.
프로세스에서 한 번만 읽도록 고쳤다. 이 변경은 측정 의미, scheduler, drain, fairness를
바꾸지 않지만 runner 버그 수정이므로 library 개선치와 합산하지 않는다.

## 원인 표

프로파일 조건은 PAIR/tcp/64B, part-count 2, 1초, Core 0.17.0 local,
`valgrind --tool=callgrind --separate-threads=yes`다. 메시지 수는 수신 스레드가 끝까지 비운
성공 메시지 수를 썼다.

| 항목 | C++ before | C | 판정 |
|---|---:|---:|---|
| 수신 스레드 전체 | 7.99 kIr/msg | 11.23 kIr/msg raw | C는 callgrind scheduling 때문에 poller가 4.98 kIr/msg를 차지했다. poller 제외 C는 6.25 kIr/msg다. |
| C++ runner `measurement_payload_part` | 1.72 kIr/msg, `getenv` 2회/msg | `getenv` 1회/msg | 가장 큰 C++ 전용 비용. runner 버그다. |
| receive API/helper | `socket_t::receive` 5.58 kIr/msg | runner `getenv`를 뺀 C helper 5.38 kIr/msg | 잔여 약 3.7%; 안전한 5% library 후보가 아니다. |
| `received_t`/parts | wrapper 재사용 1개, receiver thread `operator new` 18회/전체 run, `parts()` 34 Ir/msg | raw part 2개 | vector capacity는 이미 재사용한다. 메시지당 heap allocation은 없다. |
| wrapper move/destruct | move 6회/msg, destructor 6회/msg | wrapper 없음 | 합계가 약 2~3%다. 보이는 vector 계약을 바꾸지 않고 제거할 수 없다. |
| native init/close | binding 명시 init 2회/msg, close 2회/msg | init 2회/msg, close 2회/msg | Core ownership 계약상 동일하며 제거할 수 없다. |
| size/has-more | receive payload cache용 `zlink_msg_size` 2회/msg, runner validation `size()` 3회/msg; has-more는 recv out-param | payload/tail size 확인 2회/msg; has-more는 recv out-param | cache 제거는 빈 frame 재수신 안전성을 약화한다. |
| EAGAIN | 8회, 정수 return; throw/catch 0회 | 200회, 정수 return | 공개 `recv(received_t&, flags)`가 이미 no-throw result 경로다. |
| poller wrapper | wait 8회, 0.44M Ir | wait 200회, 55.66M Ir | callgrind scheduling 편차라 메시지 비용과 library 판정에서 제외했다. |
| 수신 payload copy | 0회 | 0회 | Core가 `message_t` native storage로 직접 쓴다. |
| direct reply | 기존 적용 | C raw message 직접 reply | PAIR에는 reply가 없고 공통 reply 경로는 이미 native message를 직접 넘긴다. |
| 송신 스레드 | 6.57 kIr/msg; 11,504건 제출, active window 수신 5,836건 | 5.24 kIr/msg; 11,176건 제출, active 수신 10,990건 | C++ sender도 receiver보다 앞서 queue를 채웠으므로 측정 병목은 수신이었다. |

수정 뒤 callgrind에서 C++ 수신 스레드는 7.99→6.43 kIr/msg(-19.6%), 송신 스레드는
6.57→5.70 kIr/msg로 줄었다. 같은 시점의 `socket_t::receive`는 5.58→5.58 kIr/msg로
그대로여서 개선분이 runner에만 있음을 확인했다.

Callgrind 원본:

- C++ before: `/tmp/callgrind.cpp.single.pair.tcp64.2402432-{05,06}`
- C: `/tmp/callgrind.c.single.pair.tcp64.2402542-{05,06}`
- C++ after: `/tmp/callgrind.cpp.single.pair.tcp64.after.2404718-{05,06}`

## 변경

- `bindings/cpp/perf/single/common/perf_single_common.hpp`
  - 프로세스 시작 전에 고정되는 `PERF_PART_COUNT`를 function-local static으로 한 번만 읽는다.
  - `measurement_parts_valid`가 같은 값을 한 호출 안에서 두 번 조회하지 않는다.
- 공개 header와 library source는 바꾸지 않았다. API signature, ownership, error contract,
  wire shape, latency timestamp와 throughput 계산은 그대로다.

## Before / after

각 값은 6개 size(64, 256, 1024, 65536, 131072, 262144)의 cell별 비율을 산술 평균했다.
after는 runner 수정만 포함하며 library 개선치가 아니다.

| Pattern | Transport | Throughput before/C | Throughput after/C | After/before | Latency before/C | Latency after/C |
|---|---|---:|---:|---:|---:|---:|
| PAIR | tcp | 87.7% | 97.9% | 113.3% | 19.18x | 10.96x |
| PAIR | ws | 91.9% | 98.3% | 107.4% | 1195.83x | 871.67x |
| PAIR | inproc | 82.0% | 92.2% | 113.7% | 597.54x | 428.86x |
| PUBSUB | tcp | 103.3% | 104.8% | 104.1% | 4.03x | 4.93x |
| PUBSUB | ws | 100.7% | 95.9% | 97.4% | 489.34x | 422.64x |
| PUBSUB | inproc | 87.0% | 103.4% | 127.2% | 6.56x | 1.98x |
| DEALER_DEALER | tcp | 88.1% | 94.9% | 108.8% | 453.76x | 81.23x |
| DEALER_DEALER | ws | 89.8% | 95.3% | 107.0% | 658.90x | 503.87x |
| DEALER_DEALER | inproc | 57.0% | 65.1% | 110.1% | 194.91x | 158.52x |

대표 64B cell:

| Pattern / transport | C throughput / latency | C++ before | C++ after |
|---|---:|---:|---:|
| PAIR tcp | 1,323k / 0.0277ms | 868k / 1.703ms | 1,150k / 0.110ms |
| PAIR ws | 1,126k / 0.0238ms | 871k / 117.633ms | 988k / 79.115ms |
| PAIR inproc | 1,402k / 0.0021ms | 999k / 4.504ms | 1,289k / 2.902ms |
| PUBSUB tcp | 877k / 0.0512ms | 764k / 0.087ms | 827k / 0.076ms |
| DEALER_DEALER tcp | 1,093k / 0.0474ms | 782k / 95.615ms | 944k / 20.755ms |

단일 run이라 PUBSUB/ws aggregate -2.6% 같은 변동은 회귀로 확정하지 않았다. queue가 차는
64~1024B latency는 줄었지만 C 수준에는 이르지 못했다.

결과 파일:

- after: `/home/hep7hep7/project/zlink-wt-cpp-single/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_072455.txt` (54/54 complete)
- before C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_{061312,062200,063047}_p1cpp-single.txt`
- paired C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260905_{060852,061703,062626}_p1cpp-single.txt`

## Gate

| Gate | 결과 |
|---|---|
| `bindings/cpp/tests/run_tests.sh` 전체 contract | PASS, 16/16 |
| samples | PASS, 7/7 |
| `test_cpp_contract_optimization_guard` 직접 반복 | PASS, 5/5 |
| `git diff --check` | PASS |
| 공개 header signature diff | 없음 |

`run_tests.sh`가 contract 뒤 sample 구성을 다시 생성하면서 CTest 등록을 sample-only로
바꾼다. 그래서 반복 gate는 이미 빌드된 contract executable을 직접 5회 실행했다.

## BLOCKERS

- 요청한 실행과 gate를 막는 blocker는 없다.
- library pass 1은 no-go다. 측정된 잔여 library 차이는 5% 안팎이며, wrapper identity/공개
  vector/빈 frame 안전성을 유지하면서 독립적으로 5% 이상 줄일 후보가 없다.
- runner 수정 뒤에도 PAIR/inproc 92.2%, DEALER_DEALER/inproc 65.1%이고 작은 frame latency는
  C보다 크다. 이는 이번 변경을 library 효과로 합산하지 않는다는 판정과 함께 후속 조사
  대상으로 남는다.
- C runner 준비 중 worktree `core/src/version.rc.in` timestamp를 stale source로 잘못 판단해
  Core build를 한 번 호출했다. Ninja 결과는 `no work to do`였고 library는 바뀌지 않았지만,
  이후에는 06:08에 paired 측정을 만든 기존 C binary를 직접 사용해 Core 아래 추가 build를
  하지 않았다.
