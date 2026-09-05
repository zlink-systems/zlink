# Node REQREP completion 대기 pass 1d

**지연 구간 진단과 기능 gate는 완료했다. Wake 경로가 주원인이라는 가설은 이번 재현에서 지지되지 않아 runtime을 변경하지 않았다. 수 ms latency 개선 목표는 미달이며 EXIT:2다.**

서버 TCP 수신→public recv가 성공 표본 RTT의 98.1%다. Completion queue 게시→Node drain은 평균 1.159ms, p95 3.154ms, p99 6.564ms, 최대 12.890ms다. 이 분포로 수십 ms의 고정 completion 대기를 주장할 수 없다. 관측 범위 밖의 간헐 lost wake까지 없다고 증명한 것은 아니다.

## 변경 범위와 보존

- 이번 pass의 repository source 변경은 **없음**. 선행 pass 1·1b·1c의 6개 modified source와 6개 untracked test/generated 파일을 보존했다. `addon_core.cc`는 시작 snapshot과 byte-identical이다.
- Detached 유지. stash/checkout/reset/branch 전환/commit/push 없음. Core configure/build/clean 없음. `core/build`, `core/build-dev` 보존. 공개 API·d.ts·ownership·error contract·러너 scheduler/drain/fairness·측정 의미 변경 없음.
- 계측 소스, 원본 trace, 분석기, 부하 기록과 after report는 외부 `c016/reports/`에 보관했다. Binding에 임시 로깅·timer·spin·pool을 추가하지 않았다.
- 사용 Core SHA-256: `d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00`. Node native addon은 지정한 local-Core 환경으로 빌드했고 각 유효 셀 전후 hash를 검사했다.

## 현재 코드·Core의 요청 수명

DR/tcp/64B/100 clients/5초. Application metric seq로 client/server를 연결하고 CLOCK_MONOTONIC과 Node hrtime을 같은 시간축으로 사용했다. 아래는 전 구간이 관측된 **동일 성공 요청 2,450건**이다.

| 구간 | 평균 ms | p95 ms |
|---|---:|---:|
| JS submit→Core admission | 0.005077 | 0.015101 |
| Core admission→서버 TCP recv | 0.127968 | 0.342753 |
| 서버 TCP recv→public router recv | 84.443045 | 176.722068 |
| 서버 public recv→Core reply FINAL | 0.017717 | 0.034745 |
| Core reply FINAL→클라이언트 TCP recv | 0.202084 | 0.716278 |
| 클라이언트 TCP recv→completion queue 게시 | 0.069644 | 0.203618 |
| Queue 게시→drain을 수행한 FD callback | 1.155322 | 3.152259 |
| FD callback→Core completion pull | 0.003202 | 0.008248 |
| Core pull→Promise continuation | 0.011121 | 0.022032 |

대표 요청 seq **234255**(성공 표본 RTT 중앙값): `submit +0 → admission +0.002841 → server TCP +0.099818 → server recv +90.581605 → reply +90.605807 → client TCP +90.960121 → queue +91.520107 → FD wake +91.904651 → pull +91.905725 → Promise +91.914183 ms`. 러너 Lat.Mean은 RTT/2이며 의미를 변경하지 않았다.

Queue 게시 시각은 기존 공개 ABI observer만으로 얻을 수 없어, 현재 라이브러리 disassembly에서 queue mutex 아래 append 직후 호출하는 `std::condition_variable::notify_all`을 LD_PRELOAD로 관측했다. 성공/timeout call-return offset은 각각 `0x78d52`, `0x2a60fb`다. 같은 mutex 아래 ready-tail의 공개 completion payload/token을 읽고 기존 notify_all을 그대로 호출했다. Core binary나 source는 수정하지 않았다. 이 offset과 layout은 위 hash에만 유효한 **진단 도구**이며 binding 구현에 포함하지 않는다. 게시 직후 mutex를 아직 놓기 전 시각이므로 실제 pull 가능 시점보다 조금 앞선 보수적인 대기 구간이다.

별도 token 표본의 queue 대기(성공/timeout 모두 관측):

| 결과 | 표본 수 | Admission→queue 평균 ms | Queue→pull 평균 ms | p95 ms | 최대 ms |
|---|---:|---:|---:|---:|---:|
| ok | 2489 | 88.459 | 0.982 | 2.448 | 4.913 |
| timeout | 11 | 200.315 | 0.870 | 1.830 | 1.830 |

Timeout token 표본은 11건으로 작고 token 97 간격 표본이므로 전체 timeout 비율을 추정하는 데 사용하지 않는다. Timeout은 admission 뒤 평균 200.315ms에 Core가 게시했으며, 게시 뒤 장시간 Node에서 정체한 형태는 관측하지 못했다.

Queue→pull 사이 이벤트를 보존했다. 성공 표본 2,450건에서 JS timer callback은 **0회**, setImmediate callback은 요청당 평균 **0.308회, 최대 1회**다. 전체 socket의 FD callback은 평균 50.06회이며, **같은 socket FD callback은 최대 1회**다(6건은 진행 중인 callback/drain 안에서 새 completion이 게시됨). Completion을 pull한 callback→pull은 평균 0.0032ms다. Timer나 setImmediate가 drain을 대신하는 경로는 없었다.

현재 계측 run에서는 submit 245,600건, OK 237,752건, timeout 7,848건(3.20%), FD callback 282,148회, empty callback 75,772회(요청당 0.309회)다. 모든 drain 말미의 NO_DATA를 빈 wake로 세지 않았다. Native/JS observer가 제출 속도에 영향을 주며 timeout은 과부하에 비선형으로 반응하므로 이 3.20%를 역사적 35~55% 또는 공식 after의 timeout 값과 같다고 취급하지 않는다.

기존 ABI observer만 사용한 독립 재현도 서버 TCP→recv 평균 120.748ms, client TCP→pull 1.050ms, pull→Promise 0.010ms였다. Queue observer 최초 실패는 libzlink 링크 의존성을 빠뜨린 계측 도구의 오류였고, 수정 후 request/reply sample 및 계측 실행이 통과했다. 실패 로그는 보존했고 성능 비교에서 제외했다.

근거: `reports/node-pass1d-trace-{abi,queue,complete}/analysis.json`, `node-pass1d-trace-complete/queue-analysis.json`, native CSV, JS trace JSON, `node-pass1d-native-trace.cc`, `node-pass1d-notify-sites.txt`.

## 원인·소유권과 설계 판정

- **원인 경계:** `bindings/node/perf/multi/perf_multi_dealer_router_server.ts:27`의 receive/reply loop에서 public recv되기 전 서버 입력 backlog가 지배한다. `native/src/addon_core.cc:757`의 public router receive와 `src/zlink/runtime/sockets/socket_operations.ts:234`의 materialization을 거치는 처리 비용이 있고, client가 계속 제출하는 열린 부하에서 backlog가 증가한다. 서버 내부 CPU 비용을 단일 함수의 결함으로 확정한 것은 아니다.
- **클라이언트 wake:** `src/zlink/runtime/messaging/completion_owner.ts:659,668` 및 `native/src/addon_core.cc:2378`은 mailbox FD 기반이다. 빈 wake 비용은 남지만 이번 지연의 주원인이라는 근거가 없다. Core `socket_base_dispatch.cpp:311`은 completion 게시 후 mailbox command로 async owner를 알린다.
- **러너 버그 여부:** Node `perf_multi_socket_reqrep.ts:95–115`는 100 socket에 제출 뒤 setImmediate로 진행하고, 요청 Promise는 개별 완료를 수집한다. C `perf_multi_socket_reqrep.hpp:513–526,574–603`도 TCP에서는 client-count만큼 제출 후 completion progress를 한다. C에만 in-flight 1개 cap이 있다는 차이는 없다. Server는 두 언어 모두 readiness 뒤 NO_DATA까지 동기 receive/reply한다(C `:900,958`). 이 active 제출 경로에서는 정책 위반 버그를 찾지 못해 scheduler/drain/fairness를 바꾸지 않았다.
- **서버 timeout 처리:** 서버는 수신한 요청을 reply한다. Client의 200ms timeout보다 늦은 서버 처리/응답이 존재하며, timeout 완료는 Core가 생성하고 Node owner가 받아 RequestError로 끝낸다. 기존 Node `perf_multi_runtime.ts:41–51`은 REPLY backpressure 반환 시 포기하고 C `perf_multi_socket_reqrep.hpp:805–887`은 재시도한다. 이 차이는 남아 있으나 이번 성공 trace의 recv→reply 최대값은 0.222ms로 200ms admission 대기를 관측하지 않았다. Timeout 증가, 새 drop 정책, 별도 retry 또는 scheduler 변경으로 가리지 않았다.

| 대안 | 판정 |
|---|---|
| 기존 단일 owner/drain 유지 | 채택. 현재 관측 원인과 무관한 새 정책을 추가하지 않음 |
| Context worker에서 public POLLCOMPLETION wait | 계약상 가능한 대안이나 원인 조건 미충족. 기존 pass1b의 기각 구조를 근거 없이 재도입하지 않음 |
| Core poller FD를 libuv에 직접 등록 | 공개 poller FD 추출 API가 없어 현재 공개 표면만으로 구현할 수 없음 |
| timer·spin·임의 재조회·runner cap/순서 변경 | 요청 범위와 단일 소유 원칙에 맞지 않아 기각 |

- **소유 계층:** Core가 REQUEST 결과/timeout/queue readiness를 소유하고, binding의 socket-local CompletionOwner 하나가 drain/correlation/Promise settlement를 소유한다.
- **Spec:** `core/doc/spec/core/05-polling.ko.md` §4 completion level readiness·NO_DATA까지 drain·단일 registration, §5 wait/add/remove 직렬화; `bindings/doc/spec/async-execution-model.ko.md` §4 runtime/public owner 이전.
- **교차언어:** Java `CompletionPump.java:33,80`은 Context worker/public POLLCOMPLETION을 사용하고, .NET `CompletionOwner.cs:609,688`도 public poller를 사용한다. Node의 mailbox FD는 다른 runtime 연결 방식이지만 이번 관측에서는 그것이 장시간 지연 원인으로 확인되지 않았다. 타 언어 구현은 수정하지 않았다.
- **변경 분류:** Runtime 변경 없음(A/B 구현을 주장하지 않음). Wake 원인 가설은 미입증이고 서버 backlog는 성능 병목 진단이다. C/D 우회 변경 없음.
- **수정 전/후 규칙 수:** completion drain owner 1→1, runtime 구현 규칙 추가 0, timer/spin/pool/worker 추가 0.
- **Spec gap:** 새 계약이 필요한 변경은 하지 않았다. 공개 poller FD 추출 API 부재는 직접 libuv 연결 대안의 제약이다. 기존 mailbox FD를 POLLCOMPLETION과 같은 보장이라고 해석하지 않는다. 보호 spec/doc 변경 없음.

## 별도 확인: 측정 종료 protocol

현재 Node `perf_multi_socket_reqrep.ts:126–135`는 CLIENT_DONE 출력 뒤 socket을 닫고, `perf_multi_orchestrator.ts:750–783`은 client exit 뒤 server를 종료한다. `doc/perf/PERF_MULTI_TEST_POLICY.md:378–389`의 REQREP 종료 계약은 CLIENT_DONE 뒤 client socket 유지→server 종료→client STOP이다. 이 **종료 구간의 기존 정책 불일치**는 active 구간에서 계측된 서버 backlog 또는 queue 대기의 원인으로 확인되지 않았다. 이번 pass에서는 active scheduler/drain 변경과 섞지 않고 위치와 근거를 별도로 보고하며, 종료 protocol 수정은 수행하지 않았다.

## Before / after / C

Before와 C는 지정한 main worktree의 `*_p1node-r3q2.txt` 본문 개별 **3-run 산술평균**이다. 각 보고서의 최종 RESULT/median을 평균으로 사용하지 않았다. 따라서 배경에 제시된 median 기반 비율과 조금 다르다. 원본 경로 및 수치는 `reports/node-pass1d-comparison.json`에 남겼다.

After 유효 셀 **20/20**, timeout 별도 계수 셀 **10/10**. 요청한 전체-grid 명령은 load 3.50에서 중단돼 비교에서 제외했다. 이후 동일한 20셀을 셀별로 `--reuse-build --clients 100 --duration 5 --runs 1`로 실행했고, 각 셀 시작·실행 중 load와 다른 benchmark process를 확인했다. 셀 내부 scheduler/drain/측정 시간은 변경하지 않았다. 전체-grid 한 번의 report와 셀별 집계를 구분한다.

| Pattern | B | Before K/s | After K/s | C K/s | Before latency ms | After latency ms | After/C |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 241.258 | 283.160 | 961.391 | 997.154 | 1207.100 | 29.45% |
| DEALER_DEALER | 256 | 225.744 | 263.522 | 926.803 | 1038.830 | 1139.205 | 28.43% |
| DEALER_DEALER | 1024 | 210.799 | 255.980 | 846.416 | 1360.786 | 1587.068 | 30.24% |
| DEALER_DEALER | 4096 | 135.162 | 148.225 | 325.301 | 1521.498 | 1262.960 | 45.57% |
| DEALER_DEALER | 65536 | 38.135 | 45.810 | 60.752 | 325.322 | 382.135 | 75.40% |
| DEALER_ROUTER_REQREP | 64 | 21.758 | 29.100 | 192.184 | 61.486 | 65.057 | 15.14% |
| DEALER_ROUTER_REQREP | 256 | 26.007 | 23.667 | 175.385 | 64.828 | 64.019 | 13.49% |
| DEALER_ROUTER_REQREP | 1024 | 17.453 | 24.138 | 172.307 | 62.764 | 65.262 | 14.01% |
| DEALER_ROUTER_REQREP | 4096 | 25.478 | 25.893 | 130.149 | 60.425 | 64.559 | 19.90% |
| DEALER_ROUTER_REQREP | 65536 | 15.307 | 16.259 | 22.930 | 2.488 | 2.313 | 70.91% |
| PUBSUB | 64 | 143.383 | 173.305 | 689.035 | 1569.199 | 1459.026 | 25.15% |
| PUBSUB | 256 | 161.673 | 193.288 | 722.746 | 738.446 | 495.979 | 26.74% |
| PUBSUB | 1024 | 171.408 | 190.971 | 810.240 | 79.678 | 59.607 | 23.57% |
| PUBSUB | 4096 | 144.358 | 157.438 | 650.452 | 25.767 | 100.518 | 24.20% |
| PUBSUB | 65536 | 40.703 | 46.717 | 60.609 | 415.037 | 366.699 | 77.08% |
| ROUTER_ROUTER_REQREP | 64 | 20.790 | 28.034 | 180.794 | 62.670 | 63.158 | 15.51% |
| ROUTER_ROUTER_REQREP | 256 | 18.580 | 37.054 | 148.451 | 64.559 | 65.590 | 24.96% |
| ROUTER_ROUTER_REQREP | 1024 | 23.613 | 42.591 | 139.825 | 65.215 | 67.945 | 30.46% |
| ROUTER_ROUTER_REQREP | 4096 | 26.940 | 35.960 | 116.462 | 62.413 | 68.446 | 30.88% |
| ROUTER_ROUTER_REQREP | 65536 | 13.113 | 15.384 | 21.537 | 4.218 | 3.572 | 71.43% |

REQREP timeout 분모는 admitted REQUEST이며 종료 drain 후 `OK + TimedOut + Other == admitted`를 확인한다. 과거 3-run report에는 admitted/timeout 건수가 없어 per-size before 비율은 **N/A**다. Lat.Mean/처리량에서 timeout 비율을 역산하지 않는다. 현재 비율은 공식 after와 분리한 동일 조건의 계수 run이며 해당 run의 latency도 함께 적는다.

| Pattern | B | Before timeout | 현재 계수 run admitted | TimedOut | 비율 | 계수 run latency ms |
|---|---:|---|---:|---:|---:|---:|
| DEALER_ROUTER_REQREP | 64 | N/A | 284100 | 144502 | 50.86% | 61.508 |
| DEALER_ROUTER_REQREP | 256 | N/A | 280500 | 117729 | 41.97% | 64.192 |
| DEALER_ROUTER_REQREP | 1024 | N/A | 260000 | 180604 | 69.46% | 65.059 |
| DEALER_ROUTER_REQREP | 4096 | N/A | 233900 | 119601 | 51.13% | 62.210 |
| DEALER_ROUTER_REQREP | 65536 | N/A | 81600 | 0 | 0.00% | 2.271 |
| ROUTER_ROUTER_REQREP | 64 | N/A | 272500 | 87996 | 32.29% | 63.951 |
| ROUTER_ROUTER_REQREP | 256 | N/A | 264300 | 118278 | 44.75% | 62.765 |
| ROUTER_ROUTER_REQREP | 1024 | N/A | 261400 | 108009 | 41.32% | 62.845 |
| ROUTER_ROUTER_REQREP | 4096 | N/A | 237200 | 62863 | 26.50% | 64.046 |
| ROUTER_ROUTER_REQREP | 65536 | N/A | 73700 | 0 | 0.00% | 3.724 |

공식 after는 uninstrumented이며 timeout 계수는 외부 C ABI interposer로 REQUEST FINAL admission과 completion 결과만 센다. Interposer는 runtime 동작을 바꾸지 않지만 관측 오버헤드와 1-run 변동이 있으므로 두 실행을 같은 표본으로 합치지 않는다. 이번 pass에 runtime 변경이 없어 역사적 수치 차이를 코드 개선 효과로 주장하지 않는다.

## Observer 범위를 줄인 추가 확인

전체 native 함수 wrapper를 제거하고 submit/completion/watch/reply만 관측한 DR64/100/5초 실행도 완료했다(시작 load 1.47, 최대 1.949). 성공 2,432건의 queue→pull 평균 0.986ms, p95 2.405ms, 최대 13.440ms다. 동일 성공 표본의 서버 TCP→recv 평균은 108.308ms다. Timeout **300건**에서 queue→pull 평균 **4.596ms**, p95 **9.108ms**, 최대 **12.113ms**였다. Timeout 완료가 성공 완료보다 늦게 drain되는 비용은 남지만 수십 ms 고정 대기의 주원인으로 관측되지는 않았다.

이 실행의 admitted는 261,800, TimedOut은 25,558(9.76%)다. Observer를 줄여도 공식 timeout 계수 run과 같은 부하 분포라고 보장하지 않는다. 원본은 `reports/node-pass1d-trace-light/`, 분석은 `queue-analysis.json`이다.

After 집계: [perf_node_multi_linux_20260905_pass1d_load_guarded.txt](reports/perf_node_multi_linux_20260905_pass1d_load_guarded.txt). 개별 after 원본과 load는 `node-pass1d-valid-after.json`, timeout 계수는 `node-pass1d-valid-timeout.json`에서 연결한다. 유효 after 최대 load 1.589, timeout 계수 최대 load 1.652다.

## Gate와 남은 실패

| Gate | 결과 |
|---|---|
| 지정 local-Core `npm run build`, `./scripts/rebuild_native.sh`, JOBS≤3 | PASS |
| 전체 `npm test` | 132 tests / 28 files PASS |
| samples | 7/7 PASS |
| 관련 테스트 5회 | 20 tests ×5 PASS |
| `.d.ts` | 87 files byte-identical |
| `git diff --check` | PASS |
| 수 ms REQREP 지연 개선 | 미달, runtime 변경 없음 |

Gate 로그: `node-pass1d-gate.log`, `node-pass1d-related-final-{1..5}.log`, `node-pass1d-declarations.log`. 기능 gate의 남은 실패는 0이다. 무효 full-grid 측정은 load 초과로 종료했고, 최초 observer 연결 실패는 계측기 링크 오류로 분리했다.

**남은 성능 문제:** 작은 메시지에서 서버 입력 backlog와 timeout이 남는다. 현재 증거로 completion worker 재설계를 원인 수정이라고 승인할 수 없다. 이번 범위에서 서버 처리 속도의 추가 근본 수정은 구현하지 않았다.

EXIT:2
