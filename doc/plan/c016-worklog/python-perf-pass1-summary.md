# Python binding hot-path pass 1

구현·기능 gate·공식 after 20셀을 완료했다. **처리량 목표 60%는 미달이며 EXIT:2**다. DD 64B의 역사적 before 대비 처리량은 15.18k→23.92k msg/s(+57.6%)다. 같은 시각에 복원 before와 이어 실행한 별도 64B 비교에서도 DD +46.2%, DR +9.2%를 관측했다. 완전 무할당 SEND는 달성하지 못했다.

재개 시 Python 미커밋 diff는 없었다. 기존 cProfile/py-spy 자료와 진행 로그를 유지했다. Detached HEAD `acd8f9104177`을 유지했으며 commit/push/reset/checkout/stash, Core build/clean을 실행하지 않았다. 기존 `core/build`, `core/build-dev` symlink도 그대로다.

## 원인과 변경

| 비용 위치 | 관측·판정 | 변경 |
|---|---|---|
| `native_support.py:220,242` 메시지 구성·clone | DD 2-part당 init_size 2회, data 1회, init/copy 각 2회가 ctypes 경계를 넘었다. 가이드 §2.1·2.4 | 기존 C 확장에서 단일 메시지 초기화·복사·clone을 처리한다. Buffer protocol로 입력을 받고 Core-owned 복사본을 만든다. Multipart submit은 합치지 않는다. |
| `routed_async.py:90,331,1026,1136` 즉시 SEND | 성공 ID 0에도 Future, private Condition, registry 등록/해제가 붙었다. 가이드 §2.1 | Future는 실제 대기 때만 생성한다. SEND는 기존 owner Condition을 공유하고, 토큰이 반환된 경우에만 registry에 등록한다. 제출과 등록을 기존 owner lock으로 연결한다. |
| `message_materializer.py:51`, `_zlink_native.c:422` 받은 part의 reply clone | DR server는 part마다 Python ctypes init 후 C copy를 호출했다. 가이드 §2.4 | 초기화·clone·실패 정리를 C의 `init_cloned_message` 한 곳으로 모았다. 본문을 bytes로 바꿔 reply하지 않는다. |
| `native_support.py:208` Message bytes snapshot | size/data ctypes 호출 뒤 Python bytes 구성 | 기존 C 확장 한 번으로 snapshot을 만든다. 독립 처리량 효과는 따로 판정하지 않았다. |
| 수신 data·retry·drain | Received data snapshot 수명, 토큰 O(1) 조회, 단일 public/runtime drain owner가 이미 존재 | 수명·제어 정책 유지. zero-copy Received view, 추가 poller·timer·scheduler·pool은 도입하지 않았다. |

변경 파일은 `bindings/python/src/zlink/_native/_zlink_native.c`, `_runtime/handles/native_support.py`, `_runtime/messaging/message_materializer.py`, `_runtime/messaging/routed_async.py`와 `bindings/python/tests/test_message_storage_bridge.py`다. 공개 contracts·ffi signature·러너·spec·문서는 수정하지 않았다.

대안은 ctypes scratch 재사용과 기존 C 확장의 단일 메시지 storage 연산이었다. Scratch 공유 수명·동시성 상태를 추가하지 않는 후자를 선택했다. 선행 WRITABLE 보류 map을 추가하는 대신 기존 owner lock으로 제출→등록→lookup을 연결했다. SEND의 native snapshot/clone은 입력 변경과 재전송 수명을 보존하기 위해 남겼다.

**수정 전/후 규칙 수:** SEND registry 등록 대상 2(즉시 성공·토큰)→1(토큰), SEND 상태 동기화 영역 2(owner·entry)→1(owner), native receive clone 초기화 소유자 2(Python·C)→1(C). 신규 timer·poller·재시도 정책 0개.

## DD/DR 64B 비용 계측

cProfile은 ctypes 함수를 계수 wrapper로 감쌌다. 아래 Python 함수 호출 수는 이 wrapper 호출을 뺀 값이다. Python 함수에는 binding·runner·stdlib를 포함하며 C built-in 호출은 제외했다. ctypes 열에는 setup/close/poll도 포함된다. 분모는 실제 DATA submit 또는 server 처리 건수이며 DD 종료용 single-part STOP 100개는 DATA 분모에서 제외했다.

| 역할 | Python 함수/건 before→after | Binding Python 함수/건 | Runner Python 함수/건 | ctypes/건 | GIL 표본 점유 before→after |
|---|---:|---:|---:|---:|---:|
| DD client | 99.95→74.67 | 51.29→35.19 | 6.08→6.06 | 11.04→4.02 | 78.3%→76.4% |
| DD server | 42.41→41.43 | 25.62→25.12 | 10.80→10.55 | 0.95→0.89 | 28.0%→31.2% |
| DR client | 162.09→154.05 | 84.30→74.30 | 9.08→9.08 | 18.02→11.03 | 81.7%→77.7% |
| DR server | 58.89→56.51 | 49.43→47.24 | 4.63→4.55 | 4.16→2.14 | 25.7%→24.0% |
| C reference | 0 | 0 | 0 Python 호출 | 0 | 해당 없음 |

C는 Python runtime을 사용하지 않으므로 Python 함수/ctypes/Python allocator가 0이라는 뜻이다. **C native 함수 수·heap allocation을 0으로 계측했다는 뜻은 아니다.** C native heap census는 수행하지 않았다.

GIL은 `py-spy --gil --subprocesses --rate 200`의 process별 sampled seconds를 해당 child 실행 시간으로 나눈 표본 추정이다. Startup/teardown과 sampling 오차를 포함하며, 정확한 CPU/GIL stall 비율이나 메시지당 GIL 시간을 뜻하지 않는다. Before 수집기는 samples 2261/errors 0의 speedscope와 정상 완료 벤치를 저장한 뒤 `No child process (os error 10)`로 exit 1했다. After 수집기는 exit 0이다. 수집기 종료 오류를 벤치 실패로 바꾸거나 숨기지 않았다.

Memray는 Python allocator를 추적했다. 아래는 **할당/재할당 이벤트의 누적 값**으로, 살아 있는 객체 수나 peak RSS가 아니다. 가장 가까운 binding/runner frame으로 귀속했으며 그 아래 stdlib 비용도 포함한다. 계수 wrapper 아래 발생한 할당은 별도 bucket이다. 해당 bucket에는 observer와 ctypes 호출 경계 비용이 섞이므로 binding 열을 전체 할당으로 해석하지 않는다. 정확한 무계측 객체 census로 주장하지 않는다.

| 역할 | Binding Python 할당 이벤트/건 before→after | Binding 누적 B/건 | Runner 이벤트/건 | 관측 전체 Python 이벤트/건* |
|---|---:|---:|---:|---:|
| DD client | 307.0→188.1 | 29156→17982 | 77.5→77.0 | 440.0→290.5 |
| DD server | 83.1→80.2 | 7356→7162 | 74.6→73.2 | 172.7→167.7 |
| DR client | 526.6→479.5 | 46979→42887 | 180.8→180.7 | 795.8→718.7 |
| DR server | 166.4→152.4 | 16547→15382 | 49.0→48.0 | 243.4→222.0 |
| C Python allocator | 0 | 0 | 0 | 0 |

\* 관측 전체에는 observer bucket이 포함된다. Native allocator 이벤트와 모든 원본 계수는 [cost census](reports/python-pass1-cost-census.json)에 따로 보존했다. 계측 처리량은 공식 성능 결과에 섞지 않았다.

공식 runner 입력은 mutable bytearray + empty tail의 2-part다. Python에서 관측되는 명시적 본문 복사는 초기 send/request의 Python→Core 1회이며 전후 동일하다. DD server와 DR server의 metric 읽기는 Received snapshot 1회를 유지한다. DD SEND retained clone 2회, DR REQUEST completion clone 2회, DR server reply clone 2회도 유지한다. `zlink_msg_copy` 호출을 full-payload memcpy로 환산하지 않았다. C 구조 대조는 `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:167`의 native part 구성 memcpy와 `common/perf_multi_socket_reqrep.hpp:901`의 native receive/reply 경로다. C에는 Python snapshot 변환이 없다. Readonly buffer의 중간 bytes 복사 제거는 별도 API 경로의 구조 개선이며 mutable 공식 입력의 copy 감소로 계산하지 않았다.

Runner의 `perf_multi_common.py:362` `send_routed`는 메시지마다 cooperative yield용 Future/call_soon을 만들고, `measurement_parts`에서 환경 변수를 조회한다. 가이드가 금지한 scheduler·fairness·tuple 사전 생성 변경은 하지 않았다. Runner 수정/효과 합산은 0건이다.

## 공식 before / after / C

TCP, clients 100, duration 5s, runs 1, sizes 64/256/1024/4096/65536, part-count 2. Core runtime SHA-256은 전후 `d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00`이며 `core/build/lib/libzlink.so.0.17.0`와 package payload가 일치한다. Extension은 지정 venv에서 `setup.py build_ext --inplace`로 빌드했다. Core는 빌드하지 않았다.

Before/C는 브리프가 지정한 19:32~19:38 원본을 main 작업 트리에서 찾아 [reports](reports/)로 복사했다. After는 요청한 전체-grid 명령을 그대로 실행한 [공식 report](reports/perf_python_multi_linux_20260905_205941.txt)다. 20/20 complete, RESULT 100/100, fail 0, max load 2.302, 다른 측정 프로세스와 겹침 없음. Load는 2초 간격으로 확인하고 3 초과 또는 외부 벤치 발견 시 해당 실행을 중단·제외했다. 최초 불완전 full-grid와 초기 진단 실행은 최종 표에 포함하지 않았다.

| Pattern | Before/C 평균 | After/C 평균 | After/C 중앙값 | After latency/C 중앙값 |
|---|---:|---:|---:|---:|
| DD | 6.60% | 9.51% | 3.32% | 0.01x |
| DR_REQREP | 14.20% | 15.59% | 8.88% | 5.93x |
| RR_REQREP | 15.37% | 17.53% | 10.19% | 6.48x |
| PUBSUB | 26.24% | 28.51% | 20.19% | 0.66x |

평균은 브리프와 맞춘 크기별 비율의 산술평균이다. 계획의 중앙값 기준으로도 네 패턴 모두 처리량 목표 60% 미달이다. PUBSUB는 이번 변경이 측정 hot path를 바꾸지 않는 control이므로 역사적 report 대비 상승을 binding 최적화 효과로 귀속하지 않는다. 1-run 전체-grid 비교에는 시각별 변동이 포함된다.

DD/PUBSUB는 Kmsg/s, REQREP는 Kops/s. Latency는 기존 runner의 metric 의미 그대로 ms다.

| Pattern | B | Before K/s | After K/s | C K/s | 증감 | After/C | Latency before→after ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| DD | 64 | 15.184 | 23.923 | 922.279 | +57.6% | 2.59% | 0.146→0.137 |
| DD | 256 | 15.389 | 22.931 | 889.213 | +49.0% | 2.58% | 0.143→0.120 |
| DD | 1024 | 15.277 | 22.830 | 688.417 | +49.4% | 3.32% | 0.143→0.122 |
| DD | 4096 | 15.187 | 22.777 | 319.173 | +50.0% | 7.14% | 0.157→0.132 |
| DD | 65536 | 13.403 | 18.899 | 59.202 | +41.0% | 31.92% | 0.208→0.214 |
| DR_REQREP | 64 | 13.180 | 14.500 | 169.401 | +10.0% | 8.56% | 6.214→5.656 |
| DR_REQREP | 256 | 13.000 | 14.300 | 170.295 | +10.0% | 8.40% | 6.309→5.727 |
| DR_REQREP | 1024 | 12.888 | 14.020 | 157.806 | +8.8% | 8.88% | 6.336→5.840 |
| DR_REQREP | 4096 | 12.500 | 13.580 | 130.626 | +8.6% | 10.40% | 6.537→6.021 |
| DR_REQREP | 65536 | 9.392 | 10.346 | 24.799 | +10.2% | 41.72% | 10.874→9.827 |
| RR_REQREP | 64 | 11.900 | 13.400 | 137.077 | +12.6% | 9.78% | 6.888→6.125 |
| RR_REQREP | 256 | 11.800 | 13.499 | 136.071 | +14.4% | 9.92% | 6.925→6.066 |
| RR_REQREP | 1024 | 11.400 | 13.080 | 128.421 | +14.7% | 10.19% | 7.162→6.241 |
| RR_REQREP | 4096 | 11.300 | 13.100 | 113.716 | +15.9% | 11.52% | 7.225→6.221 |
| RR_REQREP | 65536 | 8.683 | 9.873 | 21.346 | +13.7% | 46.25% | 12.067→10.098 |
| PUBSUB | 64 | 141.119 | 164.941 | 651.602 | +16.9% | 25.31% | 1096.307→995.154 |
| PUBSUB | 256 | 134.617 | 157.540 | 780.412 | +17.0% | 20.19% | 1175.840→393.879 |
| PUBSUB | 1024 | 123.890 | 151.957 | 902.987 | +22.7% | 16.83% | 498.173→340.557 |
| PUBSUB | 4096 | 124.693 | 133.318 | 691.127 | +6.9% | 19.29% | 1013.380→269.242 |
| PUBSUB | 65536 | 43.373 | 43.669 | 71.646 | +0.7% | 60.95% | 108.498→199.683 |

같은 시각의 64B 연속 비교는 baseline Python 소스만 `git show HEAD:<path>`로 외부 디렉터리에 복원해 실행했다. 작업 트리는 되돌리지 않았다. 복원 before/after는 같은 Core를 로드했다: DD 17.792→26.018 Kmsg/s(+46.2%), DR 13.221→14.440 Kops/s(+9.2%), max load 1.815. 이 추가 비교와 프로파일은 원인 분리용이며 공식 20셀 report를 대체하지 않는다.

## Gate와 계약

- 공식 `PYTHON_EXECUTABLE=.../python bindings/python/tests/run_tests.sh`: **208 passed + 4 subtests, samples 7/7**. [로그](python-pass1-gate-approved.log).
- storage/completion/request-writable/boundary 관련 테스트: **72개 × 5회 PASS**. [로그](python-pass1-repeat-approved.log). Mutable/readonly/strided/typed/empty/large buffer, extension-free fallback, native clone lifetime, 즉시 성공/즉시 오류의 Future 미생성, registry 미등록, 제출 반환 전 다른 스레드의 WRITABLE 수신을 포함한다.
- `git diff --check`: PASS. Public class method signature AST 전후 동일. [검사](reports/python-pass1-public-signatures.json). 공개 contracts와 runner diff 없음.
- Native C 컴파일 경고 없음. Core/package runtime hash 일치. 신규 기능 실패 없음.
- 소유 계층: Python binding의 message storage와 기존 socket completion owner. Core의 admission·target 선택·credit·WRITABLE 발행 정책을 다시 구현하지 않았다.
- Spec 근거: `core/doc/spec/core/socket/README.ko.md:938–993`의 ID 0 inline success, nonzero wait token, 소비되는 part, context 수명·WRITABLE correlation. Python ownership은 `bindings/doc/spec/python/README.ko.md:97–113`. 사용자 지정 가이드 §2.1/2.2/2.4 적용.
- 교차언어: C++ pass1의 즉시 completion map 제거, Go pass1의 기존 owner lock을 통한 submit/등록 연결과 같은 방향이다. .NET pass1은 native scratch clone을 유지한다. Python에서는 ctypes/GIL 왕복과 Python Condition/Future 생성이 추가 비용이므로 이 경계를 별도로 줄였다. C의 native receive/reply를 Python bytes 왕복으로 대체하지 않았다.
- 변경 분류: **B — 기존 binding 비용 결함**. 새 public API나 ownership/error 계약을 추가하지 않았다.

## Spec gap 여부

신규 public API/ownership/error gap은 없다. **기존 문구 불일치는 있다.** `bindings/doc/spec/python/README.ko.md:170`은 Core exact dependency를 0.16.0으로 적고, `:174` 및 `bindings/doc/spec/async-coroutine-policy.ko.md:56–64`는 native FINAL 전 provisional registry 등록을 모든 awaitable send/request에 요구한다. 이는 사용자 지정 Core 0.17 가이드 §2.1의 “토큰이 반환된 뒤에만 등록”과 맞지 않는다. 이번 구현은 사용자 지시대로 SEND의 토큰 등록을 지연하면서 기존 lock으로 선행 completion과의 경계를 보존했다. REQUEST의 선행 등록/합류는 유지했다. 문구 충돌을 새 계약으로 덮지 않았으며 보호된 spec은 수정하지 않았다. 후속 문서 검토 제안: REQUEST provisional join과 SEND inline/no-entry·WRITABLE join을 구분하고 exact dependency를 현재 계약과 맞출 것.

## BLOCKERS / 남은 미달

1. 네 패턴의 처리량 목표 60% 미달. DD는 여전히 크기와 무관한 Python 고정 비용이 크다. GIL 표본 점유는 DD client 약 76%, DR client 약 78%다.
2. 즉시 SEND의 Future·별도 Condition·registry 등록은 없앴지만, public builder, stable context를 제공하는 private entry, staging `ZlinkMsg`, retained native clone은 남는다. **전체 무할당 요구는 미충족**이다. Counter/state/pool을 더 얹어 이를 숨기지 않았다.
3. REQUEST의 필수 awaitable/registry, completion clone과 Message.data ctypes 조회, runner의 per-message yield·metric 작업이 남는다. 금지된 multipart outbound 통합, public wrapper pool, PyDLL GIL 유지, fairness 변경을 시도하지 않았다.
4. C native heap allocation census는 미수행이다. Python 함수/ctypes/Python allocator와 C 구조 대조 범위로 표를 제한했다. GIL은 sampling 추정이며 정확한 hold/stall 계측은 아니다.
5. 기존 spec의 dependency/provisional 등록 문구 충돌은 별도 정합 검토가 필요하다. 이후 pass 2의 후보 검토는 이 pass 1 보고를 입력으로 판단해야 한다.

재현 자료: [전체 비교 JSON](reports/python-pass1-comparison.json), [비용 census](reports/python-pass1-cost-census.json), `reports/python-pass1-final-{cprofile,memray,gil}-{before,after}/`, `reports/python-pass1-*-load.json`. 계측·분석 스크립트는 동일 c016 디렉터리의 `python-pass1-{capture,measure,analyze,compare}.py`에 보존했다. 보고서의 Python 함수 수는 계측 wrapper 제외 값이며 진행 중 공유한 raw cProfile 수(DD 111→78.69)는 wrapper 포함 값이다.

EXIT:2
