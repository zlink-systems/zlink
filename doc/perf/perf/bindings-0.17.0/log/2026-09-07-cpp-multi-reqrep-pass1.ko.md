# C++ Multi TCP REQREP pass 1 — 2026-09-07

## 판정

성능 후보는 **기각하고 원복했다**. REQUEST completion의 합류 상태를 단순화해 capture 명령 수를
줄였지만 처리량 개선이 확인되지 않았고, 대표 회귀 셀의 latency 한도를 넘었다. 이 pass를
성능 목표 통과나 library 개선 채택으로 보고하지 않는다. Core·binding 공개 API·perf runner는
변경하지 않는다. 최종 source diff는 이 로그 한 파일이다. Commit·push는 하지 않았다.

## 고정 조건과 자료

- 작업 branch: `main`, 기준 report의 commit `31c5e4f7f0`. 작업 중 문서 commit
  `937b9affa3`이 추가됐으나 두 commit 사이 C/C++ runner와 C++ binding source diff는 없다.
- 시작 시 기존 변경: `doc/perf/PERF_MULTI_TEST_POLICY.md`, 성능 개선 계획서,
  `framework/languages/{cpp,node}/bench/with-grpc/log/smoke*`의 untracked 자료. 보존했다.
- Core: `ZLINK_CORE_SOURCE=release`,
  `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1`.
- Runtime: 위 prefix의 `lib/libzlink.so.0.17.1`, revision
  `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`, SHA-256
  `a3e00fd269b2a1c8d66371ac7ae7efd3f6dd25e6ab842484b847352258ea39a2`.
- Host: WSL2 / Intel Core Ultra 7 265K / logical CPUs 20. C++ Release, GCC 13.3,
  binding LTO OFF. Core build·교체 없음.
- 공식 조건: TCP, clients 100, I/O threads 4/4, auto-HWM balanced,
  2-part, sizes 64/256/1024/4096/65536, duration 5 s, runs 1.
- 실행 전마다 `bash scripts/perf/wait-for-idle-perf.sh`와 load 확인, perf는 직렬 실행.
  시작 `uptime` load 1.68/1.99/1.16, 후보 after 0.35/0.76/0.90,
  회귀 before 0.35/1.08/1.01. 진단 실행 로그의 1분 load도 5 미만이었다.
  같은 host·boot·Core이고 시작 load가 기존 baseline의 허용 범위 안이어서,
  공식 C 기준은 요청받은 `p5cmodel`을 유지했다. 축소 C 측정은 진단용이다.
- 진단 harness·callgrind·trace·빌드/테스트 로그:
  `/tmp/zlink-cpp-reqrep-pass1/`. `pair.py`는 기존 바이너리를 직접 실행하며
  `READY → CLIENT_DONE → server STOP/종료 → client STOP`을 지킨다.
  양쪽 exit 0인 경우만 JSON에 `status: complete`를 기록한다.

### C 기준과 후보 report

- `MULTI_DEALER_ROUTER_REQREP` C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_220942_p5cmodel.txt`
- `MULTI_DEALER_ROUTER_REQREP` C++ before: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_221009_p5cmodel.txt`
- `MULTI_ROUTER_ROUTER_REQREP` C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_221036_p5cmodel.txt`
- `MULTI_ROUTER_ROUTER_REQREP` C++ before: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_221103_p5cmodel.txt`
- 후보 after: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_222318_reqrep1-candidate.txt` — complete, 10/10.

## 진단: 두 현상의 관계

**두 현상은 C++ requester에서 발생하지만, 같은 비례 비용 하나로 설명되지 않는다.**
64 B에서는 native submit 비용이 거의 같은데 coroutine·completion 변환을 포함한 application
명령 수가 늘었다. 64 KiB에서는 admission 전 대기와 반복된 WRITABLE 재제출이 지연의 대부분을
차지한다. 크기별 처리량 비율의 평평함만으로 payload 크기에 비례하는 단일 비용을 확정할 수 없다.

### 교차 pairing

기존 C/C++ 바이너리, TCP, **clients 100**, duration 2 s, 2-part. 모든 조합 complete.
최초 위치 분리는 clients 10에서 수행하고, 후보 원복 뒤 공식 client 수 100에서 확인했다.
양쪽 source는 p5cmodel과 동일하다. 아래는 역할 분리용 진단이며 공식 aggregate에는 합산하지 않는다.
표 안 값은 **ops/s / mean latency(ms, RTT/2)**다.

| Pattern | Size | C server + C client | C++ server + C client | C server + C++ client | C++ server + C++ client |
|---|---:|---:|---:|---:|---:|
| DR | 64 | 386,350.0 / 0.234548 | 389,424.5 / 0.246000 | 296,748.0 / 0.226819 | 282,082.0 / 0.265980 |
| DR | 65536 | 62,060.0 / 0.723791 | 61,788.5 / 0.797508 | 46,759.5 / 6.098505 | 40,422.5 / 9.460583 |
| RR | 64 | 353,709.5 / 0.219643 | 345,236.0 / 0.226078 | 281,613.0 / 0.224445 | 272,414.0 / 0.238687 |
| RR | 65536 | 55,342.5 / 0.857580 | 58,012.0 / 0.731405 | 48,274.0 / 4.039586 | 47,507.0 / 3.886800 |

C client를 유지하면 C++ server의 64 B 처리량은 C server의 DR 100.8%, RR 97.6%다.
64 KiB도 C++ server + C client의 latency는 0.798/0.731 ms다. C++ client를 쓰면 server
언어와 무관하게 64 KiB latency가 3.9~9.5 ms로 오른다. 따라서 server recv/reply가 격차의
주 병목이라는 가설은 기각한다. Server 언어에 따른 변동도 남아 있으므로 server 비용이
전혀 없다는 뜻은 아니다.

최초 clients 10 진단도 같은 방향이다. DR/RR 64 B의 C++ server + C client 처리량은
C+C의 99.2%/94.7%, 64 KiB C++ client latency는 두 server에서 모두 10~13 ms였다.
진단 JSON 이름은 `<server>_<client>_<pattern>_<size>_<clients>_2.json`이다.
예: `python3 /tmp/zlink-cpp-reqrep-pass1/pair.py c cpp dealer_router_reqrep 65536 100 2`.

### Callgrind: 64 B

DR, clients 4, duration 2 s. client·server 양쪽을 callgrind로 실행해 한쪽만 느려져
과부하가 생기는 조건을 줄였다. `--tool=callgrind --cache-sim=no --collect-jumps=no`.
정규화 분모는 native 2-part REQUEST 시도 수(C 20,052, C++ 10,920)다. 이 셀의
C++ `async()`·`submit_request_attempt()`·`capture()` 횟수는 모두 10,920으로 재시도가 없었다.
프로세스 전체 수치는 setup·Core I/O thread를 포함하며, allocation을 binding만의 비용으로
오해하지 않도록 전체와 호출 경로를 구분했다.

| 64 B / request | C | C++ before | C++/C |
|---|---:|---:|---:|
| client 전체 Ir | 17,360 | 26,886 | 1.55x |
| application window 포함 Ir | 12,790 | 20,114 | 1.57x |
| `zlink_request_part` 포함 Ir | 6,343 | 6,477 | 1.02x |
| client `operator new` calls | 0.626 | 6.408 | 10.23x |
| client `malloc` calls (`new`가 호출한 것 포함) | 3.054 | 9.398 | 3.08x |
| server 전체 Ir | 15,623 | 22,619 | 1.45x |
| server `operator new` calls | 0.497 | 2.295 | 4.62x |

`malloc`과 `operator new`는 겹치므로 합산하지 않는다. C++ application window에서 native
request 호출을 뺀 비용은 약 13.6k Ir/request, C는 약 6.4k다. 이 차이는 coroutine 생성·재개,
result/entry, public reply vector, scheduler closure와 binding completion drain을 포함한다.
6.4회의 `new`가 모두 completion bundle에서 발생하는 것은 아니다.

선행 DD pass의 즉시 admission 최적화는 SEND 전용이다
(`src/Runtime/Messaging/send_operations.cpp`, `immediate_send_result_t`).
REQREP의 `request_reply.cpp:115`는 여전히 bundle을 만들지만 이는 단순 누락이 아니다.
`bindings/doc/spec/README.ko.md:1337`과 async-coroutine-policy §1은 REQUEST의 성공 admission을
nonzero REQUEST ID와 reply/timeout까지 유지할 결과에 연결한다. SEND의 ID 0처럼 bundle을
버리면 request terminal 계약을 어긴다. Server reply는 synchronous terminal이므로 이 SEND
최적화를 호출하지 않는다.

### Callgrind: 65536 B

DR, clients 2, duration 1 s, client만 callgrind, server는 native 속도.
C/C++ 모두 complete인 축소 실행만 사용했다. Core reply 완료 경로는 C 609건,
C++ final settlement는 432건이다(C++ active 안의 유효 완료는 236건).

| client 전체 관측 | C | C++ before |
|---|---:|---:|
| 전체 Ir | 116,994,364 | 276,592,692 |
| native REQUEST record 시도(2-part calls / 2) | 1,213 | 21,316 |
| 완료된 request당 native 시도 | 1.99 | 49.34 |
| 완료된 request당 전체 Ir | 192,109 | 640,261 |
| 전체 `operator new` calls | 6,193 | 72,080 |
| 완료된 request당 `operator new` calls | 10.17 | 166.85 |

C++ `async()` 432회 대 `submit_request_attempt()` 21,316회다. 프로파일러가 backpressure를
증폭하므로 49.34회를 native 실행의 재시도율로 일반화하지 않는다. 아래 native 시각 계측으로
실제 지연 구간을 별도로 확인했다.

초기 server-only callgrind는 C client의 retained-request drain timeout으로 종료됐다.
양쪽 callgrind의 C++ 64 KiB/clients 4도 complete가 아니었다. 해당 결과는 위 표·성능 판정에
쓰지 않았다. Timeout·HWM은 변경하지 않고, 진단용 축소 셀에서 유효 profile을 확보했다.

### 요청 수명 계측

DR, clients 100, duration 2 s, native 속도. `/tmp/.../trace.cpp`의 LD_PRELOAD probe가
공개 native API 경계에서 metric timestamp·sequence를 읽어 seq % 128 == 0만 기록했다.
Core source·binary와 runner는 수정하지 않았다. 이 임시 probe는 공식 perf에 사용하지 않는다.

| 평균 구간(ms, RTT/2가 아닌 실제 경과 시간) | C 64 B | C++ 64 B | C 64 KiB | C++ 64 KiB |
|---|---:|---:|---:|---:|
| runner stamp → 성공 admission 반환 | 0.001294 | 0.001705 | 0.358334 | **9.228659** |
| admission 반환 → server public recv | 0.169994 | 0.145942 | 0.162527 | 0.390537 |
| server recv → reply 제출 반환 | 0.002191 | 0.002490 | 0.006935 | 0.003516 |
| reply 제출 반환 → client completion pull | 0.270740 | 0.291102 | 0.975495 | 1.830121 |
| stamp → completion pull | 0.444219 | 0.441238 | 1.503291 | **11.452832** |
| 모든 구간이 연결된 표본 수 | 5,800 | 4,400 | 887 | 700 |

64 KiB C++의 admission 전 구간은 전체의 **80.6%**다. 같은 표본에서 BACKPRESSURED 반환은
C 239회/성공 887건, C++ 3,223회/성공 700건이었다. 성공까지의 평균 시도는 1.27 대 5.60회다.
64 B 표본에는 양쪽 모두 BACKPRESSURED가 없었다. Kernel wire timestamp를 계측하지 않았으므로
admission→recv에는 송신 queue·wire·server queue가, reply→pull에는 reply 전송과 completion
대기가 함께 포함된다. Coroutine 재개·latency sampler는 pull 이후이므로 이 표에 포함하지 않는다.

원인은 admission 전 작업이 쌓이는 실행 형태와 그 재제출 비용의 결합이다.
C `perf_multi_socket_reqrep.hpp:233`은 retained request가 있으면 그 slot의 새 제출을 건너뛰고,
C++ `perf_multi_reqrep.hpp:232`·`:351`는 매 turn 각 socket에 새 async request를 시작한다.
C++ binding은 각 operation을 보존하고 exact WRITABLE에만 재제출하며
(`completion_owner.cpp:212`, `:572`), 크기가 커져 credit이 부족하면 동일 socket에서 여러
pre-admission operation이 경쟁한다. 이 차이는 reply in-flight 상한으로 해결할 수 없다.
이미 승인된 제거를 되돌리는 상한·타이머·선별 재시도·재제출 순서 변경은 하지 않았다.

이전 64 KiB 실패의 completion-owner 문제와는 구분된다. 현재 requester 전체가 단일 public
poller의 `pollcompletion`에 등록돼 있다(`perf_multi_reqrep.hpp:314`). `poller.cpp:529`의 drain은
그 wait caller에서 실행한다. 교차 실행과 공식 10개 셀은 모두 성공했고 EINVAL/same_thread 충돌을
관찰하지 않았다. Owner 충돌의 잔재라고 판정할 근거는 없다.

## 검토한 설계와 판정

필독 가이드 §4, decisions D-B121~D-B130·D-BP1~16, DD pass·64 KiB 실패 기록,
2026-09-05 C++ pass 2의 no-go와 대조했다. 기존 no-go는 재구현하지 않았다.

| 후보 | 판정과 근거 |
|---|---|
| REQUEST completion의 중복 합류 상태 제거 | 구현·측정 후 **기각**. 아래 결과와 회귀 gate 참조 |
| REQUEST에 SEND의 ID 0 즉시 완료 bundle 제거 적용 | request terminal은 reply까지 유지해야 하므로 계약 불일치, 구현 안 함 |
| entry/result 전체 통합 | caller detach/abandon와 Core context 수명이 다르다. 기존 단일 bundle allocation을 더 없애지 못하면서 동기·비동기 책임을 크게 재구성해야 하므로 이번 근거로 채택 안 함 |
| large-message buffer pool | 확정 no-go, 구현 안 함 |
| public result/entry/coroutine frame pool | 기존 ABA·identity·ownership no-go, 구현 안 함 |
| scheduler 함수 포인터화 | 기존 public header/ABI no-go, 구현 안 함 |
| await-ready fast path·native 2-part inline staging·PMR map 확대·reply 직접 adopt | 이미 적용된 후보, 중복 구현 안 함 |
| snapshot 없이 native part 소비 | BACKPRESSURED 후 exact payload 보존 계약 위반, 구현 안 함 |
| cap·timeout 증가·sleep·clients 축소로 공식 수치 개선 | 금지. clients 축소는 명시적으로 허용된 진단 profile에만 사용 |
| runner의 blocking/nonblocking wait 변경 | library 개선에 합산 불가. 아래 정책 적용 범위 판단 항목으로 분리 |

### 구현했던 후보의 POSDDD 검토

변경 전 `capture()`는 `_published`를 기다린다. 그런데 `publish()`와
`settle_if_joined()`는 별도로 `_captured`와 `_published`의 양방향 합류를 다시 판단했다.
이 실행 순서 의존과 중복 상태가 위험 신호였다. 얕은 새 adapter나 공개 설정을 추가하는 방향은
피하고, (A) 기존 entry가 publication 대기·최종 capture를 한 번에 소유하는 방안과
(B) result/entry 전체를 통합하는 방안을 비교했다. 시험한 것은 A다.

A는 `_captured`·미사용 `wait_settled()`를 제거하고 REQUEST의 확인→변환→settle을 같은 lock에서
진행한다. Async continuation을 호출하기 전에는 lock을 해제한다. Completion entry의 lock
왕복을 요청당 2쌍 제거했고, 합류 규칙 수는 **2 → 1**이었다. 공개 interface·Core token 판정·
NO_DATA 뒤 exact retry 순서는 보존했다. Source는 17행 추가/37행 삭제였고 새 상태는 없었다.

같은 64 B callgrind에서 capture inclusive Ir/request는 1,809.8 → 1,635.6(**−9.6%**),
application 전체는 20,114 → 19,873(**−1.2%**)였다. Allocation 횟수는 사실상 같았다.
전송·대기 구간 전체를 줄이는 효과는 없었으며 아래 공식 처리량과 회귀 측정으로 기각했다.
`/tmp/zlink-cpp-reqrep-pass1/rejected-candidate.patch`에 시험 diff를 보존했다.

- 소유 계층: binding의 submit/completion 합류와 언어 terminal 전달.
- 계약: async-execution-model §4·§5, README Submit 결과 투영, async-coroutine-policy §1.
- 교차언어: C는 slot/context로 완료를 집계하며, C++은 coroutine result 수명을 추가로 보존한다.
  Wire/admission 정책은 같은 Core가 소유한다. Framework 변경은 없다.
- 분류: B 후보(기존 중복 구현 정리), **성능·회귀 gate 기각 후 원복**.
- 최종 규칙 수: 원복으로 변경 전과 동일. 후보의 2→1을 최종 개선으로 집계하지 않는다.

## 후보 after — 기각된 코드의 측정

C는 `p5cmodel`, 같은 공식 조건이다. Aggregate는 size별 비율의 산술평균이다.
이 표는 기각 후보의 증거이며 최종 source 성능으로 표시하지 않는다.

| Pattern | Size | C ops/s | C++ before | 후보 after | after/C | latency/C |
|---|---:|---:|---:|---:|---:|---:|
| DR | 64 | 396,765.2 | 297,839.0 | 297,765.4 | 75.05% | 1.056x |
| DR | 256 | 387,091.2 | 271,640.8 | 266,307.6 | 68.80% | 0.997x |
| DR | 1024 | 377,700.4 | 273,040.0 | 263,091.0 | 69.66% | 0.912x |
| DR | 4096 | 344,984.2 | 254,785.6 | 251,080.2 | 72.78% | 0.807x |
| DR | 65536 | 62,438.0 | 45,556.2 | 44,711.4 | 71.61% | 9.619x |
| **DEALER_ROUTER_REQREP aggregate** | — | — | — | — | **71.58%** | **2.678x** |
| RR | 64 | 357,692.0 | 271,557.4 | 273,711.6 | 76.52% | 1.079x |
| RR | 256 | 346,240.0 | 258,961.2 | 258,340.0 | 74.61% | 1.160x |
| RR | 1024 | 333,540.0 | 249,726.4 | 249,523.2 | 74.81% | 1.078x |
| RR | 4096 | 308,332.4 | 235,980.0 | 231,806.4 | 75.18% | 1.081x |
| RR | 65536 | 58,040.4 | 46,345.2 | 45,800.6 | 78.91% | 5.983x |
| **ROUTER_ROUTER_REQREP aggregate** | — | — | — | — | **76.01%** | **2.076x** |

목표는 throughput ≥85%, latency ≤2.0x. 후보는 두 pattern 모두 미달이다.

## 회귀 gate

TCP, clients 100, sizes 64/1024, duration 2 s, runs 1. Before/후보 모두 complete 4/4.

- Before: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_222028_reqrep1-reg-before.txt`
- 후보 after: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_223138_reqrep1-reg-after.txt`

| Pattern | Size | Before ops/s | 후보 ops/s | throughput 변화 | Before latency(ms) | 후보 latency(ms) | latency 변화 |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,411,034.5 | 1,370,698.5 | -2.86% | 0.056875 | 0.072498 | +27.47% |
| DEALER_DEALER | 1024 | 1,232,884.5 | 1,236,959.5 | +0.33% | 0.554734 | 0.516204 | -6.95% |
| PUBSUB | 64 | 1,886,026.0 | 2,004,216.5 | +6.27% | 474.356896 | 504.749620 | +6.41% |
| PUBSUB | 1024 | 2,334,550.0 | 2,340,412.5 | +0.25% | 578.231451 | 511.679886 | -11.51% |

DD 64 B mean latency **+27.47%**로 허용치 +10%를 넘었다. 이 경로에 후보가 직접 영향을
준다는 인과까지 확정하지는 않았다. 이 회귀 after 첫 셀 부근에는 임시 publication 진단의
소형 C++ compile/link 약 1초도 겹쳤다. 다른 perf나 Core 빌드는 없었지만 측정 잡음 가능성을
기록한다. 처리량 이득이 확인되지 않고 채택 gate도 충족하지 못했으므로 후보를 기각했다.
아래 원복 상태에서도 DD latency 한도 초과가 관찰돼 후보 때문에 생긴 회귀로 단정할 수 없다.

기능 검증: 관련 5 tests 통과, `bindings/cpp/tests/run_tests.sh` contract **19/19**·sample
**7/7** 통과, Release 복원 뒤 send-close stress **1/1** 통과. 별도 unit label은 없으며
binding 단위 동작 검증은 contract suite에 포함된다. 최종 원복 후 검증 결과는 아래에 덧붙인다.

## 별도 발견과 감독자 판단

1. **blocking REQUEST publication wake 누락**: 원래 `completion_owner.cpp:280-285`의
   `publish()`는 `_published=true` 뒤 `settle_if_joined()`만 부른다. Early completion의
   `capture()`는 `_changed.wait()`에서 `_published`를 기다리므로 `_captured`를 아직 설정하지
   못하고, `settle_if_joined()`는 반환해 notification이 없다. Blocking submit과 조기 drain이
   이 순서로 겹치면 대기한다. 이번 perf의 async 경로는 `submit_request_attempt()`가 직접
   notify하므로 이 발견을 REQREP 성능 격차의 원인으로 합산하지 않는다.
   원본 `completion_owner.cpp/.hpp`를 임시 object로 컴파일하고 condition_variable::wait
   진입을 linker wrap한 결정적 진단 `/tmp/.../publication.cpp`에서
   `FAIL: publication did not wake the early completion`을 재현했다.
   이 pass에서는 관련 없는 blocking 경로 수정과 test를 남기지 않았다. 별도 correctness 수정 판단이 필요하다.
2. **runner wait 조항 적용 범위**: `perf_multi_reqrep.hpp:325-340`은 매 active submit turn에
   최대 50 ms bounded wait를 사용한다. C는 새 제출 성공 시 wait 0, 전부 blocked일 때 bounded wait다.
   PERF_MULTI_TEST_POLICY §1.3.1에는 requester의 bounded wait 허용과 event-loop alignment의
   turn당 wait 0이 함께 있다. 현재 C++은 coroutine ready queue와 public poller를 함께 쓰므로
   어느 조항을 적용할지 감독자 확인이 필요하다. 사용자가 이미 수정 중인 정책과 p5cmodel 조건을
   바꾸지 않았다. 변경이 필요하면 runner 정합 항목으로 분리하고 before를 새로 잡아야 한다.
3. **64 KiB pre-admission 경쟁**: 공개 REQUEST awaitable은 admission 대기와 reply 대기를
   내부에서 처리한다. C의 retained slot과 같은 admission 경계를 application에서 복제하거나
   reply 완료를 새 제출의 gate로 쓰지 않았다. 현재 계약 아래 일반 경로의 재제출 비용을 줄이는
   후속 후보가 필요하며, 상한 복원·perf 전용 API로 해결하지 않는다.
4. 본 pass는 후보 하나의 측정·기각이다. 전체 캠페인의 최종 `보류`나 목표 통과를 대신 판정하지 않는다.

## 최종 원복 확인

- `bindings/cpp/{src,include,tests}`의 `git diff`는 모두 **0줄**이다. 공개 헤더 diff도 **0줄**.
- 원복 뒤 Release로 재빌드하고 contract **19/19**, sample **7/7**, stress **1/1**,
  합계 **27/27** 통과. `restored-tests.log`에 보존했다.
- Core SHA-256은 시작 기준과 일치한다. Core·Framework·타 언어 binding 수정 없음.
- 최종 REQREP report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_223715_reqrep1-final-restored.txt`.
  **complete 10/10**, duration 5 s, runs 1, `uptime` load 0.22/0.44/0.70.
- 최종 대표 회귀 report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_223811_reqrep1-reg-restored.txt`.
  **complete 4/4**, duration 2 s, runs 1, `uptime` load 3.47/1.28/0.97.
  측정 중 compile·다른 perf 없음.

### 최종 상태 측정 — 성능 변경 원복 후

동일한 원래 코드를 다시 측정한 값이다. 기각 후보를 원복했다는 확인용이며 개선 효과로
합산하지 않는다. C 기준은 여전히 p5cmodel이다.

| Pattern | Size | C++ ops/s | C 대비 | Mean latency(ms) | C 대비 latency |
|---|---:|---:|---:|---:|---:|

| DR | 64 | 295,182.4 | 74.40% | 0.247672 | 1.068x |
| DR | 256 | 275,546.6 | 71.18% | 0.241103 | 0.996x |
| DR | 1024 | 256,805.4 | 67.99% | 0.239909 | 0.968x |
| DR | 4096 | 250,018.8 | 72.47% | 0.257733 | 0.831x |
| DR | 65536 | 44,938.8 | 71.97% | 7.079486 | 9.482x |
| **DEALER_ROUTER_REQREP aggregate** | — | — | **71.60%** | — | **2.669x** |
| RR | 64 | 274,761.0 | 76.81% | 0.246853 | 1.106x |
| RR | 256 | 256,282.2 | 74.02% | 0.239193 | 1.094x |
| RR | 1024 | 248,901.4 | 74.62% | 0.244022 | 1.127x |
| RR | 4096 | 234,780.0 | 76.15% | 0.261844 | 1.149x |
| RR | 65536 | 45,010.0 | 77.55% | 5.314362 | 6.759x |
| **ROUTER_ROUTER_REQREP aggregate** | — | — | **75.83%** | — | **2.247x** |

### 원복 후 대표 회귀 확인

비교 기준은 변경 전 `reqrep1-reg-before`, 동일한 duration 2 s다.

| Pattern | Size | 원복 후 ops/s | throughput 변화 | 원복 후 latency(ms) | latency 변화 |
|---|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,403,633.5 | -0.52% | 0.076254 | +34.07% |
| DEALER_DEALER | 1024 | 1,285,490.0 | +4.27% | 0.493083 | -11.11% |
| PUBSUB | 64 | 2,010,622.0 | +6.61% | 494.588236 | +4.27% |
| PUBSUB | 1024 | 2,218,001.0 | -4.99% | 512.871034 | -11.30% |

원복 후에도 DD 64 B mean latency가 **+34.07%**여서 지정 회귀 gate를 통과로 보고할 수 없다.
최종 library source diff가 0이므로 이를 새 코드가 유발한 결함이라고 분류하지 않는다.
처리량은 네 셀 모두 −5% 한도 안이다(PUBSUB 1024 B −4.99%). 이 pass는 기능 검증을
통과했지만 성능 개선·회귀 gate 통과·목표 달성을 주장하지 않으며, 추가 library 변경을 남기지 않는다.

최종 변경 파일: 이 로그만. 최종 공개 헤더 diff 0줄. Commit/push 없음.
