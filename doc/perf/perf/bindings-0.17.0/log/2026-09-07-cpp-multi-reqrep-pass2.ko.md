# C++ Multi TCP REQREP pass 2 — 2026-09-07

## 판정

**후보를 기각하고 원복했다.** 64 B의 continuation 중복 참조를 줄이는 새 후보는 기능 검증을
통과했지만 DR 성능과 대표 회귀 gate를 통과하지 못했다. 64 KiB의 첫 requester socket 표본에서는
WRITABLE 이후 C++의 재제출 경과 시간이 C보다 짧았으며, 중간 목록 제거로 반복 거절을 해결할 근거가 없었다.

pass 1·2 뒤 이 범위에서 채택 가능한 계약 보존 후보가 남지 않아 두 pattern의 TCP 판정은
**`보류`**로 기록한다(계획서 §7.4 16단계). 이는 성능 목표 통과 판정이 아니다.
이 기록이 감독자 Claude의 검토·판정 자료이며 정책·계획서·decisions는 수정하지 않는다.
최종 원복 상태의 재검증은 아래에 기록한다.

## 고정 조건과 변경 범위

- Branch `main`, 시작 commit `3f0e914468`. `31c5e4f7f0`부터 이 commit까지
  `bindings/cpp`·`bindings/c/perf`의 source diff는 없다.
- 기존 변경 `doc/perf/PERF_MULTI_TEST_POLICY.md`와 Framework의 untracked
  `bench/with-grpc/log/smoke*` 자료를 보존한다.
- Core는 `ZLINK_CORE_SOURCE=release`,
  `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1`로 고정했다.
  Runtime revision은 `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`, SHA-256은
  `a3e00fd269b2a1c8d66371ac7ae7efd3f6dd25e6ab842484b847352258ea39a2`다.
- WSL2, Intel Core Ultra 7 265K, logical CPUs 20, C++ Release, binding LTO OFF.
  공식 조건은 clients 100, I/O threads 4/4, balanced auto-HWM, TCP, 2-part,
  sizes 64/256/1024/4096/65536, duration 5 s, runs 1이다.
- 모든 perf 실행 전 `bash scripts/perf/wait-for-idle-perf.sh`와 `uptime`을 실행한다.
  load가 5를 넘으면 기다리며, perf와 compile은 겹치지 않는다. C 기준은 요청된
  `p5cmodel` report를 유지한다.
- 진단·빌드·검증 자료는 `/tmp/zlink-cpp-reqrep-pass2/`에 보존한다. 공식 perf에는
  진단용 `LD_PRELOAD`를 설정하지 않는다.
- D-BP17의 blocking REQUEST `publish()` wake 누락은 별도 correctness 항목이며 수정하지 않는다.

## 64 KiB 재제출 경로 대조

### 코드와 소유권

C++은 WRITABLE에서 coroutine을 재개하지 않는다. 같은 public poller `wait()`의 drain 안에서
해당 entry를 찾고, queue를 `NO_DATA`까지 비운 뒤 native REQUEST를 재제출한다. Coroutine과
scheduler는 REQUEST의 최종 reply/error가 도착했을 때 실행된다.

| 단계 | C requester | C++ requester |
|---|---|---|
| 거절 입력 보관 | socket slot의 payload·wait token·retry-ready | operation이 message parts를 소유하고 completion entry가 context·token을 보유 |
| 깨어나는 주체 | 단일 native poller를 호출하는 application thread | public poller에 이전된 단일 completion owner, 즉 `wait()` 호출 thread |
| WRITABLE 식별 | slot의 token·context 대조 | owner의 inline entry 또는 PMR map에서 context 조회 후 entry의 exact token 대조 |
| drain 중 보관 | slot의 `retry_ready` 표시 | call-local `vector<shared_ptr<completion_entry_t>>`에 entry 추가 |
| 재제출 시점 | ready socket들을 drain한 뒤 다음 submit turn에서 retained slot을 제출 | 해당 socket에서 `NO_DATA`를 관측한 뒤 vector 순서로 `entry->retry()` 호출 |
| payload 준비 | `init_size`와 `memcpy`로 보관 payload를 native part에 복사 | 보관한 C++ message에서 refcount를 공유하는 native view 구성, native staging은 이미 stack 사용 |
| 다시 거절된 경우 | 같은 slot이 새 token을 기다림 | 같은 operation이 새 token을 기다림. 결과·coroutine은 그대로 pending |
| 새 operation | retained request가 있는 slot의 새 요청을 건너뜀 | 각 runner turn에서 새 `async()`를 호출하며 binding은 각 operation을 독립 보관 |

C의 요청 경로는 `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp`의
`submit_request()`(218~349, retry 핵심 233~339), `drain_socket_completions()`(352~413), active loop(576~603)다.
지시된 `perf_multi_dealer_dealer_client.cpp`의 `submit_retained_message()`(158~216)와
`drain_dd_writable()`(272~306)도 exact token을 읽고 `NO_DATA` 뒤 제출하는 같은 계열이다.
DD helper 자체를 REQREP 실행 함수로 오인하지 않았다.

C++ 경로는 `poller.cpp:529` → `completion_owner.cpp:572`의 drain → `capture():298` →
`retries.push_back():658` → `NO_DATA` 분기의 `entry->retry():604` →
`submit_request_attempt():212` → `operation_submit.hpp:145`의 native 변환이다.
Retry 때 entry를 새로 만들거나 map에 다시 등록하지 않는다. 새 token만 같은 entry에 게시한다.

계약 근거는 다음과 같다.

- `bindings/doc/spec/README.ko.md:1337-1343`: REQUEST ID는 reply까지 유지하며,
  socket-local context·token의 WRITABLE을 해당 waiter로 전달한다.
- `bindings/doc/spec/README.ko.md:1346-1350`: part API binding은 송신 경로에 자체 lock이나
  gate를 두지 않는다. Admission FIFO로 첫 시도·재제출을 제어하는 정책은 이 소유권을 침범한다.
- `bindings/doc/spec/async-execution-model.ko.md` §4·§5: 단일 completion owner,
  public poller로 owner 이전, submit/completion 합류와 정확히 한 번의 정리.
- `core/doc/spec/core/socket/README.ko.md:1071-1087`: DONTWAIT는 admission을 한 번 시도한다.
  거절된 payload는 caller가 보관하고, exact WRITABLE 뒤 `NO_DATA`까지 drain한 후 같은 요청을
  재제출한다. 재제출이 다시 거절되면 새 token으로 기다린다.
- 같은 Core 문서 `:1028`: WRITABLE은 다시 submit할 수 있다는 신호이며 payload admission
  완료가 아니다. Binding이 신호를 다른 operation에 배분하거나 token 없는 operation을
  대신 재제출할 근거가 되지 않는다.

### Native 경계 계측

pass 1의 `pair.py`를 `/tmp`에서 재사용하고, 공개 `zlink_request_part`·
`zlink_completion_recv`·`zlink_poller_wait`만 interpose했다. 첫 requester socket의 모든
attempt·WRITABLE·REQUEST completion을 메모리에 보관하고 종료 후 파일로 출력했다.
Core·binding·runner source를 바꾸지 않았으며, overflow는 모두 0이다.

TCP, clients 100, 65536 B, 2-part, duration 2 s. C+C와 C+++C++의 DR/RR 모두
`status: complete`다. 아래 시간은 RTT/2가 아닌 실제 경과 시간이다. 첫 socket은 처리 순서상
유리할 수 있으므로 이 표의 재시도율·대기 깊이를 전체 100 sockets의 평균으로 일반화하지 않는다.
전체 client에서 admission 전 대기가 우세하다는 근거는 pass 1의 전체 socket 표본을 유지한다.

| 관측 항목 | C DR | C++ DR | C RR | C++ RR |
|---|---:|---:|---:|---:|
| 표본 socket의 완료 request | 1,623 | 924 | 1,432 | 879 |
| native REQUEST attempts | 1,701 | 3,671 | 1,462 | 1,754 |
| WRITABLE records | 78 | 2,747 | 30 | 875 |
| request당 attempts | 1.048 | 3.973 | 1.021 | 1.995 |
| WRITABLE 뒤 retry admission 성공률 | 100.0% | 33.42% | 100.0% | 100.0% |
| WRITABLE 반환 → 다음 native attempt 시작, 평균 µs | 86.35 | 10.16 | 100.62 | 1.52 |
| 같은 구간 p95 µs | 194.72 | 20.09 | 155.13 | 2.43 |
| 같은 구간의 추가 poll 호출, 평균 | 0.013 | 0 | 0 | 0 |
| BACKPRESSURED 반환 → WRITABLE 반환, 평균 ms | 1.028 | 1.830 | 1.371 | 1.306 |
| 이 거절→재제출 주기의 poll 호출, 평균 | 1.013 | 1.003 | 1.000 | 1.000 |
| 보관 중인 pre-admission operation 최대 수 | 1 | 3 | 1 | 1 |
| runner stamp → 성공 admission, 평균 ms | 0.066 | 5.501 | 0.044 | 1.331 |
| native 2-part attempt, 평균 µs | 6.97 | 4.78 | 8.15 | 9.01 |

Poll 호출은 공개 API 관측값이다. Kernel wakeup·context switch 횟수를 측정한 것은 아니다.
첫 request 계측 시작 뒤 client process의 native poll 호출 수는 C DR 1,706 / C++ DR 978, C RR 1,469 / C++ RR 941이다.
C++은 poll 호출 사이에서 더 많은 operation을 제출·처리하며, WRITABLE 하나 때문에 scheduler나
추가 poll을 거치는 현상은 관측하지 않았다. 모든 retry에서 앞선 token·context와 해당
WRITABLE의 일치, 보존된 stamp·sequence의 동일성을 확인했다.

DR 표본에서는 새 요청이 계속 들어오는 동안 여러 pre-admission 요청이 반복해서 거절된다.
RR의 첫 socket 표본은 최대 대기가 1개여도 거의 모든 새 요청이 한 번 거절돼 다음 progress
turn까지 기다렸다. 따라서 두 pattern 전체를 고정 대기 깊이 하나로 설명하지 않는다.
이 첫 socket 표본에서는 신호를 받은 뒤의 C++ 재제출 자체가 C보다 오래 걸린다는 가설은
지지되지 않는다.

### Retry 목록의 비용

새 callgrind 실행 없이 pass 1의 유효한 65536 B profile을 재분석했다.
`completion_owner_t::drain()`에서 retry vector 확장에 들어가는 `operator new`와 대응
`operator delete`는 각각 2,871회, inclusive Ir는 1,506,786과 289,480이다.
전체 client 276,592,692 Ir의 **0.65%**다. 72,080회의 전체 `new`를 retry vector나
completion bundle의 할당으로 합산하면 잘못된 후보를 고르게 된다.

같은 profile에서 `drain → entry->retry()`는 191,872,600 Ir(69.37%)이며 여기에는 반복된
native REQUEST가 포함된다. 0.65%는 allocator 호출의 명령 비중으로, 실제 시간 개선의 상한을
뜻하지 않는다. 다만 vector allocator를 바꾸는 것과 반복 거절을 없애는 것은 별개임을 보여준다.

## 후보 검토

| 원인 | 새 검토 방향 | 판단 |
|---|---|---|
| 64 B coroutine·completion 비용 | 이미 bundle 안에 있는 continuation slot으로 abandon을 처리해 weak lookup 중복 제거 | 구현·기능 검증 뒤 **기각·원복**. DR 악화와 대표 회귀 gate 미충족 |
| 64 KiB retry 중간 자료구조 | call-local retry vector를 owner의 reusable scratch 또는 intrusive FIFO로 교체 | 미채택. token·시도 수·wake를 줄이지 못하며 allocator Ir 비중 0.65%. Scratch 반환·종료 시 참조 해제 또는 link 수명을 추가해야 한다 |
| 64 KiB 거절 반복 | socket별 pre-admission FIFO로 새 제출과 재제출을 조절 | 기각. 첫 DONTWAIT 시도와 exact WRITABLE별 재제출에 binding의 별도 admission·순서 규칙을 추가한다 |
| 64 KiB 재제출 시각 | WRITABLE capture 즉시 제출 | 기각. `NO_DATA` 뒤 재제출 계약을 어긴다 |

64 B의 1 request당 allocation은 pass 1 원자료에서 application coroutine, REQUEST bundle,
reply vector, scheduler callable, resume callable로 나뉜다. Bundle과 reply vector는 각각
1회이며 retry vector 할당은 이 64 B profile에 없다. 기존에 기각한 public 객체 pool,
REQUEST의 ID 0 즉시 완료 취급, 공개 scheduler ABI 변경, capture/publish 합류 단순화는
다시 구현하지 않는다.

### 기각한 continuation 후보의 계약과 POSDDD 검토

위험 신호는 inline slot과 별도 heap slot의 저장 방식, 그리고 scheduler로 이동한 slot을 다시
찾기 위한 `_continuation_weak`다. Production REQUEST와 backpressured SEND는 이미 각각의
bundle에 result와 inline slot을 두고 `bind_lifetime(bundle)`을 호출한다
(`request_reply.cpp:114-118`, `send_operations.cpp:90-94`). 이 경로에 별도 slot 위치를
지원하고 추적할 이유가 없다.

대안을 비교했다.

- Weak 참조만 제거하고 heap fallback 유지: **기각**. `finish()`가 `_continuation`을 scheduler로
  이동한 뒤 `abandon()`이 queued slot을 찾을 수 없어 파괴된 coroutine을 재개할 수 있다.
- Bound inline slot만 사용: **시험 후 기각**. `suspend()`는 lifetime owner가 없는 내부 오용을
  consumer 등록 전에 거부한다. 기존 aliasing shared pointer는 유지해 queued callable이
  bundle 수명을 보존하고, `abandon()`은 awaiter가 소유한 result의 inline slot을 직접 비운다.
  `_continuation_weak`와 별도 heap slot fallback을 함께 제거한다.

`operation_contracts.hpp:74-77,105-106`의 awaiter는 `abandon()` 호출 중 shared state를
소유한다. `resume_async_slot()`의 scheduler 없음·동기 실행·예외 fallback과 slot의
CAS/exchange는 바꾸지 않는다. REQUEST의 entry/result 구분과 publish/capture 합류 상태도
유지한다. Standalone internal test는 `suspend()` 없이 capture/take만 사용한다
(`test_cpp_contract_request_reply.cpp:227-241`).

계획서 §7.4 11단계에 따라 Sol/high read-only 리뷰를 수행했다. 감독 역할의 본 agent가 인용한
production 생성 경로, awaiter와 slot 수명, spec을 직접 확인한 뒤 B 후보로 시험을 승인했다.
구현 diff에 대한 Sol의 후속 리뷰에도 correctness finding이 없었다. 성능 채택 판정은 별도다.

- 소유 계층: C++ binding의 caller continuation 수명과 language terminal 전달.
- Spec: async-execution-model §5·§6의 합류·caller detach/abandon와 정확히 한 번의 정리,
  async-coroutine-policy §1·§3의 request 최종 완료 경계.
- 교차언어 대조: C requester는 slot/context로 직접 완료를 집계하므로 C++ coroutine awaiter의
  queued abandon에 대응하는 참조가 없다. Core의 token·admission·retry 규칙은 그대로다.
  Framework runtime 변경은 없다.
- 분류: B 후보, 기존 내부 중복 소유권 bookkeeping 정리.
- 후보 규칙 수: continuation 저장 방식 **2 → 1**; 새 상태 없음. Typed/void 각각 weak
  pointer 한 개를 제거한다. 공개 결과 identity·coroutine frame·entry를 pool하지 않는다.
- 후보 source diff: `async_operation_state.hpp` 한 파일, 20행 추가/38행 삭제.

## 기능 검증

변경 전 관련 contract 5/5가 통과했다. 후보 Release build 뒤
`ctest --test-dir bindings/cpp/build --output-on-failure -E '^perf_cpp_'`는
**27/27**(contract 19, sample-smoke 7, stress 1) 통과했다. 별도 unit label은 없으며
binding 단위 동작 검증은 contract suite에 포함된다. Test build에는 `-UNDEBUG`가 적용돼 있다.
Core runtime은 고정 prefix의 library를 동적으로 연결한다.

기존 suite에 더해 `/tmp/zlink-cpp-reqrep-pass2/continuation_check.cpp`의 공개 API harness로
다음을 typed REQUEST와 backpressured SEND 각각 5회 검증했다. Before와 후보 모두 **60/60**다.

- 정상 queued continuation 실행.
- 최종 completion 전 coroutine 파괴 후 late completion drain.
- scheduler queue에 들어간 뒤 coroutine 파괴 후 queued callable 실행.
- Scheduler가 enqueue 전에 throw.
- Scheduler가 enqueue한 뒤 throw해 fallback으로 재개한 다음 queued callable 실행.
- Scheduler 없이 즉시 continuation 재개.

새 API나 private binding 함수는 호출하지 않는다. Public poller와 메시지 송수신으로 순서를
확정하며 sleep을 추가하지 않았다. 이 harness의 초기 compile에서 reply builder의 lvalue
요구를 맞춘 뒤 실행했다. Runtime/test 실패를 숨기기 위한 조건 변경은 없다.

## 성능 측정

대표 회귀 before는 `reqrep2-reg-before` report이며 complete 4/4다.
C 기준 report는 다음 두 파일이다. 대응 C++ before는 같은 `p5cmodel` tag를 사용한다.

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_220942_p5cmodel.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_221036_p5cmodel.txt`

시작 load(1/5/15분)는 회귀 before **0.04/0.24/0.57**, 후보 after
**0.10/0.13/0.32**, 후보 회귀 **2.17/0.76/0.53**이었다. 진단 DR C/C++는
**0.21/0.29/0.56**, **0.14/0.26/0.53**에서 시작했다. 다른 진단 실행도 1분 load가 5
미만이었다. 모든 시작 `uptime`은 `/tmp/zlink-cpp-reqrep-pass2/*.log`에 보존했다.
후보 after는 성능 기각의 근거이며 최종 source 성능으로 표시하지 않는다.

### 후보 after

공식 C 기준은 `p5cmodel`, C++ before도 동일한 `p5cmodel`이다.
Aggregate는 size별 비율의 산술평균이며 mean latency는 RTT/2(ms)다.

| Pattern | Size | C++ before ops/s | 후보 ops/s | 후보/C | 후보 mean latency(ms) | latency/C | before 대비 throughput | before 대비 latency |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| DR | 64 | 297,839.0 | 282,021.6 | 71.08% | 0.252326 | 1.088x | -5.31% | +3.06% |
| DR | 256 | 271,640.8 | 265,320.2 | 68.54% | 0.248468 | 1.027x | -2.33% | +2.43% |
| DR | 1024 | 273,040.0 | 258,318.6 | 68.39% | 0.245598 | 0.991x | -5.39% | +7.94% |
| DR | 4096 | 254,785.6 | 255,120.2 | 73.95% | 0.254392 | 0.820x | +0.13% | +4.08% |
| DR | 65536 | 45,556.2 | 40,465.0 | 64.81% | 9.209132 | 12.335x | -11.18% | +27.49% |
| **DEALER_ROUTER_REQREP aggregate** | — | — | — | **69.35%** | — | **3.252x** | — | — |
| RR | 64 | 271,557.4 | 270,822.6 | 75.71% | 0.243214 | 1.089x | -0.27% | +2.68% |
| RR | 256 | 258,961.2 | 257,500.2 | 74.37% | 0.254505 | 1.164x | -0.56% | +3.32% |
| RR | 1024 | 249,726.4 | 248,461.6 | 74.49% | 0.247379 | 1.142x | -0.51% | +0.63% |
| RR | 4096 | 235,980.0 | 235,244.8 | 76.30% | 0.245737 | 1.078x | -0.31% | +0.24% |
| RR | 65536 | 46,345.2 | 47,065.8 | 81.09% | 4.302430 | 5.472x | +1.55% | -13.96% |
| **ROUTER_ROUTER_REQREP aggregate** | — | — | — | **76.39%** | — | **1.989x** | — | — |

### 대표 회귀

TCP, clients 100, sizes 64/1024, duration 2 s, runs 1. Before와 후보 모두 complete 4/4다.

| Pattern | Size | Before ops/s | 후보 ops/s | throughput 변화 | Before latency(ms) | 후보 latency(ms) | latency 변화 |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,406,571.0 | 1,388,420.0 | -1.29% | 0.058635 | 0.055589 | -5.19% |
| DEALER_DEALER | 1024 | 1,290,719.5 | 1,292,521.5 | +0.14% | 0.469867 | 0.481609 | +2.50% |
| PUBSUB | 64 | 2,027,567.5 | 1,919,521.5 | -5.33% | 475.927869 | 504.470505 | +6.00% |
| PUBSUB | 1024 | 2,343,184.0 | 2,328,202.0 | -0.64% | 523.768467 | 515.959685 | -1.49% |

Report 경로:

- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_230305_reqrep2-candidate.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_225020_reqrep2-reg-before.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_230401_reqrep2-reg-candidate.txt`

### 후보 기각 근거

DR은 before **72.87% / 2.688x → 후보 69.35% / 3.252x**, RR은
**76.39% / 2.152x → 후보 76.39% / 1.989x**다. RR latency aggregate는 목표 안이지만
두 pattern의 throughput aggregate가 모두 85% 미만이다. DR 64 B·1024 B throughput이
각각 −5.31%·−5.39%, 65536 B가 −11.18%다.

대표 셀은 PUBSUB 64 B throughput **−5.33%**로 −5% gate를 넘었다. 이 경로는 후보의
continuation 변경을 직접 사용하지 않으므로 후보와 이 감소 사이의 인과는 확정하지 않는다.
다만 같은 조건의 채택 gate가 충족되지 않았고 DR 개선도 없으므로 POSDDD 단순화만으로
후보를 남기지 않았다. 변동값을 이유로 같은 후보를 재측정하지 않는다.

최종 library 규칙 수는 원복으로 **2 → 2**이며, 후보의 **2 → 1**을 채택 이득으로
합산하지 않는다. 시험 patch는 `/tmp/zlink-cpp-reqrep-pass2/rejected-candidate.patch`에
보존했다. 새 wrapper·pool·timer·limit을 추가하지 않았다.

## 원복 검증

후보 patch를 보존한 뒤 원래 header 내용으로 돌리고 C++ Release를 재빌드했다.
Contract **19/19**, sample **7/7**, stress **1/1**, 합계 **27/27** 통과했다.
`restored-build.log`, `restored-tests.log`에 보존했다. Core 재빌드·교체는 없다.

최종 REQREP report는 **complete 10/10**, 대표 회귀 report는 **complete 4/4**다.
시작 load(1/5/15분)는 각각 **0.65/0.75/0.56**, **2.96/1.36/0.78**이다.
측정 중 compile·다른 perf를 실행하지 않았다. 아래 값은 원래 source의 재측정이며
기각 후보의 개선 효과로 합산하지 않는다.

### 최종 원복 상태

공식 C 기준은 `p5cmodel`, C++ before도 동일한 `p5cmodel`이다.
Aggregate는 size별 비율의 산술평균이며 mean latency는 RTT/2(ms)다.

| Pattern | Size | C++ before ops/s | 원복 후 ops/s | 원복 후/C | 원복 후 mean latency(ms) | latency/C | before 대비 throughput | before 대비 latency |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| DR | 64 | 297,839.0 | 282,363.2 | 71.17% | 0.254627 | 1.098x | -5.20% | +4.00% |
| DR | 256 | 271,640.8 | 276,240.2 | 71.36% | 0.250098 | 1.033x | +1.69% | +3.10% |
| DR | 1024 | 273,040.0 | 263,669.8 | 69.81% | 0.231060 | 0.932x | -3.43% | +1.55% |
| DR | 4096 | 254,785.6 | 250,597.4 | 72.64% | 0.260771 | 0.841x | -1.64% | +6.69% |
| DR | 65536 | 45,556.2 | 43,425.8 | 69.55% | 8.177197 | 10.953x | -4.68% | +13.20% |
| **DEALER_ROUTER_REQREP aggregate** | — | — | — | **70.91%** | — | **2.971x** | — | — |
| RR | 64 | 271,557.4 | 272,140.4 | 76.08% | 0.249383 | 1.117x | +0.21% | +5.28% |
| RR | 256 | 258,961.2 | 252,761.8 | 73.00% | 0.254683 | 1.165x | -2.39% | +3.39% |
| RR | 1024 | 249,726.4 | 249,586.6 | 74.83% | 0.240102 | 1.109x | -0.06% | -2.33% |
| RR | 4096 | 235,980.0 | 234,209.6 | 75.96% | 0.263914 | 1.158x | -0.75% | +7.65% |
| RR | 65536 | 46,345.2 | 43,722.8 | 75.33% | 5.915737 | 7.524x | -5.66% | +18.31% |
| **ROUTER_ROUTER_REQREP aggregate** | — | — | — | **75.04%** | — | **2.414x** | — | — |

### 원복 후 대표 회귀

TCP, clients 100, sizes 64/1024, duration 2 s, runs 1. Before와 원복 후 모두 complete 4/4다.

| Pattern | Size | Before ops/s | 원복 후 ops/s | throughput 변화 | Before latency(ms) | 원복 후 latency(ms) | latency 변화 |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,406,571.0 | 1,375,638.5 | -2.20% | 0.058635 | 0.067769 | +15.58% |
| DEALER_DEALER | 1024 | 1,290,719.5 | 1,219,408.0 | -5.52% | 0.469867 | 0.587338 | +25.00% |
| PUBSUB | 64 | 2,027,567.5 | 2,033,828.5 | +0.31% | 475.927869 | 488.769810 | +2.70% |
| PUBSUB | 1024 | 2,343,184.0 | 2,324,447.0 | -0.80% | 523.768467 | 531.567455 | +1.49% |

Report 경로:

- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_230617_reqrep2-final-restored.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_225020_reqrep2-reg-before.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_230713_reqrep2-reg-restored.txt`

원복 후에도 DEALER_DEALER 64 B latency **+15.58%**, 1024 B throughput **−5.52%**와
latency **+25.00%**가 허용치를 넘었다. 따라서 최종 대표 회귀 gate도 **미통과**다.
Library source diff가 0이므로 이를 새 코드가 유발한 회귀라고 판정하지 않는다.
1-run 측정의 변동을 분리할 수 없으며, 통과 수치가 나올 때까지 반복하지 않았다.

## 최종 변경과 판정 범위

- 최종 변경 파일은 이 pass 2 기록 한 파일이다. `bindings/cpp/{src,include,tests}` diff는
  모두 **0줄**, 공개 헤더 diff도 **0줄**이다.
- 기존 정책 문서 변경과 untracked Framework 자료는 시작 시 저장한 SHA-256과 일치한다.
  Core·Framework·타 언어·runner·spec·계획서·decisions를 수정하지 않았다.
- 고정 Core library의 최종 SHA-256은 시작 기준과 같다. Commit·push는 하지 않았다.
- 기능 gate는 통과했다. 성능 목표와 최종 대표 회귀 gate는 미통과이며, 두 REQREP TCP
  pattern은 **`보류`**다.
- 이 판정은 pass 1의 기각 후보와 pass 2에서 실제 검토·측정한 후보의 범위에 한정한다.
  모든 미래 최적화의 불가능성을 증명한 것은 아니다. 현재 소유권·공개 헤더·retry·runner
  제약 안에서 채택 가능한 후보가 없으므로 새 queue 정책이나 상한을 억지로 추가하지 않는다.

문서의 코드 부합·서술 원칙을 Sol/high가 별도로 검토했다. 표본 범위, poll 계측 시작점,
C 함수 범위와 FIFO 금지의 직접 spec 근거에 대한 지적은 원문을 직접 대조한 뒤 반영했다.
측정 수치·report 경로·complete 상태·최종 source diff·기능 gate 수에는 오류가 없었다.
