# Node 서버 수신 경로 pass 1c

## 결과

**기능 변경·gate·최종 20셀 측정은 완료했다. 64~4096B REQREP의 수 ms latency 목표는 미달했다. 성능 목표 판정 FAIL, EXIT:2.**

서버는 수정 전부터 POLLIN 뒤 EAGAIN까지 동기 drain하고 각 수신과 같은 JavaScript 실행 구간에서 direct reply를 제출했다. 메시지당 worker/uv_async 왕복도 없었다. 해당 제어를 새로 만들거나 러너의 scheduler·drain·fairness를 바꾸지 않았다.

채택한 변경은 multipart 라우팅 속성의 중복 생성, 내부 reply의 routing Buffer 재복사, ReplyToken owner/value 중복 저장을 제거한다. 공개·내부 생성 d.ts 87개는 byte-identical이다. 기존 Message wrapper 재사용은 유지했으며 이번 성과로 계산하지 않는다.

최종 공식 after는 **고정 Core, 20/20 complete, RESULT 100/100, fail 0**이다. 요청한 전체-grid 명령은 load가 3.36으로 상승해 중단했고 비교에서 제외했다. 이후 같은 pattern/size 20셀을 각각 `--reuse-build --clients 100 --duration 5 --runs 1 --transports tcp`로 실행했다. 셀 사이의 부하 대기는 측정 구간 밖에서만 수행했다. 아래 집계는 한 번의 전체-grid runner report가 아니라, 개별 runner 원본의 RESULT를 합친 파일이다.

- 최종 after 집계: [perf_node_multi_linux_20260905_pass1c_load_guarded.txt](reports/perf_node_multi_linux_20260905_pass1c_load_guarded.txt)
- 원본 20개와 실행 로그 목록: [node-pass1c-final-cells.json](reports/node-pass1c-final-cells.json)
- 상세 수치: [node-pass1c-comparison.json](reports/node-pass1c-comparison.json)
- 고정 Core: `reports/node-pass1c-pinned-core/libzlink.so.0.17.0`, SHA-256 `e680b264822a92f770769a37ab9df152b342413189cfb4b148404c1f5ed9b4ea`.

## 서버 수신 경로와 C 대조

| 항목 | Node 변경 전 → 후 | C reference 구조 |
|---|---|---|
| recv 실행 | JS main thread → 동기 N-API → `zlink_router_recv_part`, 동일 | app thread에서 public Core recv 호출 |
| readiness 뒤 수신 | batch 앞 cooperative yield 1회, `receiveAndReply`에서 EAGAIN까지 반복, 동일 | readiness 확인 후 `reply_one_request`를 drained까지 반복 |
| reply | 각 recv 직후 public `Received.reply().submit()`, 동일 | 각 recv 직후 `zlink_reply_part` |
| 수신용 worker 왕복 | 없음 → 없음 | 없음 |
| 서버 uv_async_send / message | 0 → 0 (프로세스 전체 interposer 계측) | Node/libuv 경계 없음 |
| Message wrapper | 기존 pool에서 재사용, 새 pool 없음 | stack native message 사용, JS wrapper 없음 |
| multipart metadata | part별 생성 → 메시지당 immutable 객체 하나 | 첫 source RID로 바로 reply |

근거: `bindings/node/perf/multi/perf_multi_dealer_router_server.ts:27,102`, `perf_multi_router_router_server.ts:27,103`, `src/zlink/runtime/sockets/socket_operations.ts:234`, `native/src/addon_core.cc:757,3732`; C는 `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:900,958`. Node의 yield는 **메시지별이 아니라 drain batch 앞**에 있다. C 셀의 wake 수를 소급 계측한 것은 아니며 C 열은 코드 구조 대조다.

## 고정 Core의 변경 전후 계측

DR/tcp/64B/100 clients/5초, 동일한 inspector sampling·N-API 계수·C ABI interposer로 비교했다. Before package는 커밋 `6c2cb784c0`의 pass1b 구현을 별도 경로에서 재구성했다. 현재 소스의 추가 변경을 섞지 않았으며 checkout/reset 없이 비교했다. 두 package 모두 고정 Core를 실제로 로딩했는지 프로세스 mapping과 hash로 확인했다.

| 항목 | Before | After |
|---|---:|---:|
| 서버 성공 recv | 187,878 | 218,175 |
| 최대 recv / tick | 187,508 | 217,513 |
| 최대 recv / readiness drain | 187,508 | 217,513 |
| recv가 있는 drain 수 | 61 | 70 |
| 전체 drain 종료 계수 (빈/오류 포함) | 16,877 | 8,188 |
| wrapper acquire 호출 | 375,756 | 436,350 |
| 실제 관측 Message wrapper identity 수 | 2 | 2 |
| 추정 JS 할당 bytes / 성공 recv | 2791.9 | 2408.1 |
| addon recv 평균 µs / 호출 | 10.299 | 9.145 |
| addon reply 평균 µs / 호출 | 12.603 | 11.351 |
| client submit | 204,700 | 226,300 |
| OK completion | 48,840 | 100,383 |
| TimedOut completion | 155,860 | 125,917 |
| Timeout 비율 | 76.14% | 55.64% |

할당은 GC로 회수된 객체를 포함한 V8 sampling 추정치다. 시작·종료, 오류, observer의 할당도 포함하므로 정확한 hot-path allocation census로 해석하지 않는다. 빈/오류 drain에는 endpoint 종료 구간도 포함된다. 큰 batch가 실제 수신 대부분을 처리한다는 사실은 확인되지만, 전체 drain 평균을 active 구간의 평균 batch 크기로 사용하지 않는다. 같은 1-run 비교에도 observer 오버헤드와 비선형 timeout 효과가 있으므로 역사적 처리량 차이 전부를 library 변경만의 인과 효과로 주장하지 않는다.

2-part routed reply에서 코드상 제거한 생성은 라우팅 properties 객체 1개와 string 생성 호출 1회, ReplyToken의 별도 owner/value 객체 1개, 내부 routing Buffer 복사 1회다. 공개 parts array와 Message Buffer 소유권은 유지한다. 계측 원본과 Core 함수별 시간: [node-pass1c-final-diagnostics.json](reports/node-pass1c-final-diagnostics.json), `reports/node-pass1c-final-matched-{before,after}/`.

### Core 큐 추이

전용 입력 메시지 깊이 옵션은 확인되지 않았다. 대신 기존 `Context.getCoreHwmBudgetSnapshot()`에 대응하는 public Core ABI snapshot을 1024회 수신마다 관측했다. 서버 Context에는 receive queue 100개와 send queue 100개가 있으며, 아래 값은 이들을 합친 bytes다. 입력 메시지 개수나 입력 큐만의 깊이로 환산하지 않는다. Completion accounted bytes는 이 서버 표본에서 0이다.

| 첫 표본 이후 ms | 누적 recv | Context core queue bytes |
|---:|---:|---:|
| 0.0 | 1,024 | 141,312 |
| 992.4 | 59,392 | 1,366,272 |
| 1994.6 | 115,712 | 1,437,696 |
| 3006.2 | 173,056 | 2,248,320 |
| 4000.2 | 226,304 | 1,603,968 |
| 4993.3 | 279,552 | 2,121,216 |

관측 peak accounted bytes는 **2,635,200**다. 상세: `reports/node-pass1c-final-matched-queue/`. 임시 계측은 모두 외부 report 도구이며 binding 소스에 로깅을 남기지 않았다.

## 최종 before / after / C

Before는 지정한 pass1 after `perf_node_multi_linux_20260905_143333.txt`다. C는 보존된 `reports/perf_c_multi_linux_20260905_135{418,525,650,755}_p1node.txt` 원본이다. 현재 worktree의 C results 디렉터리는 없어 이 동일 이름의 보존본을 사용했다. C와 Before는 역사적 기준이며 고정 Core에서 다시 실행한 결과가 아니다. 비율 요약은 size별 비율의 산술평균이다.

| Pattern | Before/C | After/C |
|---|---:|---:|
| MULTI_DEALER_DEALER | 41.20% | 41.71% |
| MULTI_DEALER_ROUTER_REQREP | 20.31% | 28.72% |
| MULTI_ROUTER_ROUTER_REQREP | 18.05% | 31.12% |
| MULTI_PUBSUB | 31.03% | 34.77% |

DD/PUBSUB는 Kmsg/s, REQREP는 Kops/s다. REQREP latency는 기존 metric 의미인 RTT/2를 유지한다.

| Pattern | B | Before K/s | After K/s | C K/s | After/Before | After/C | Latency before→after ms | Timeout %* |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| DD | 64 | 290.708 | 275.621 | 1017.379 | -5.2% | 27.09% | 1023.060→1304.087 | — |
| DD | 256 | 260.249 | 269.495 | 955.021 | +3.6% | 28.22% | 1287.034→1188.492 | — |
| DD | 1024 | 236.411 | 233.742 | 832.374 | -1.1% | 28.08% | 1407.398→1289.632 | — |
| DD | 4096 | 148.269 | 154.620 | 315.562 | +4.3% | 49.00% | 1449.510→1357.397 | — |
| DD | 65536 | 45.400 | 46.228 | 60.698 | +1.8% | 76.16% | 366.656→396.024 | — |
| DR REQREP | 64 | 8.582 | 26.915 | 145.258 | +213.6% | 18.53% | 57.439→54.702 | 48.66 |
| DR REQREP | 256 | 9.141 | 22.992 | 163.473 | +151.5% | 14.06% | 52.186→63.292 | 38.82 |
| DR REQREP | 1024 | 7.925 | 24.518 | 155.282 | +209.4% | 15.79% | 59.302→62.899 | 49.83 |
| DR REQREP | 4096 | 13.913 | 23.709 | 122.937 | +70.4% | 19.29% | 56.297→60.861 | 54.35 |
| DR REQREP | 65536 | 16.334 | 16.852 | 22.189 | +3.2% | 75.95% | 2.973→2.077 | 0.00 |
| RR REQREP | 64 | 9.399 | 26.548 | 176.436 | +182.5% | 15.05% | 61.790→62.384 | 48.91 |
| RR REQREP | 256 | 7.388 | 20.980 | 144.377 | +184.0% | 14.53% | 55.168→64.340 | 42.05 |
| RR REQREP | 1024 | 6.014 | 27.984 | 139.640 | +365.3% | 20.04% | 56.354→60.255 | 43.02 |
| RR REQREP | 4096 | 6.248 | 39.358 | 113.689 | +529.9% | 34.62% | 56.991→66.353 | 35.03 |
| RR REQREP | 65536 | 15.066 | 15.356 | 21.515 | +1.9% | 71.38% | 4.196→3.039 | 0.00 |
| PUBSUB | 64 | 158.159 | 177.356 | 588.072 | +12.1% | 30.16% | 1494.076→1594.845 | — |
| PUBSUB | 256 | 172.446 | 195.495 | 777.996 | +13.4% | 25.13% | 324.433→503.380 | — |
| PUBSUB | 1024 | 170.656 | 192.516 | 849.159 | +12.8% | 22.67% | 61.940→60.172 | — |
| PUBSUB | 4096 | 148.604 | 164.105 | 646.535 | +10.4% | 25.38% | 35.077→41.469 | — |
| PUBSUB | 65536 | 43.072 | 48.234 | 68.383 | +12.0% | 70.53% | 406.249→404.744 | — |


`Timeout %*`는 **동일 고정 Core·동일 조건의 별도 진단 run**에서 센 값이다. 공식 처리량과 latency 셀에 계수기 오버헤드를 넣지 않기 위해 분리했다. 공식 latency와 timeout 비율이 같은 run의 표본이라는 의미는 아니다. 별도 진단은 public request completion 결과만 계수하며 scheduler·제출량·timeout 설정을 바꾸지 않는다.

| Pattern | B | OK | TimedOut | Total | Timeout % | 진단 mean latency ms |
|---|---:|---:|---:|---:|---:|---:|
| MULTI_DEALER_ROUTER_REQREP | 64 | 133,680 | 126,720 | 260,400 | 48.66 | 63.838 |
| MULTI_DEALER_ROUTER_REQREP | 256 | 154,040 | 97,760 | 251,800 | 38.82 | 65.093 |
| MULTI_DEALER_ROUTER_REQREP | 1024 | 126,626 | 125,774 | 252,400 | 49.83 | 62.627 |
| MULTI_DEALER_ROUTER_REQREP | 4096 | 99,340 | 118,260 | 217,600 | 54.35 | 57.775 |
| MULTI_DEALER_ROUTER_REQREP | 65536 | 80,400 | 0 | 80,400 | 0.00 | 2.089 |
| MULTI_ROUTER_ROUTER_REQREP | 64 | 132,791 | 127,109 | 259,900 | 48.91 | 65.520 |
| MULTI_ROUTER_ROUTER_REQREP | 256 | 154,319 | 111,981 | 266,300 | 42.05 | 64.698 |
| MULTI_ROUTER_ROUTER_REQREP | 1024 | 143,082 | 108,018 | 251,100 | 43.02 | 66.260 |
| MULTI_ROUTER_ROUTER_REQREP | 4096 | 150,208 | 80,992 | 231,200 | 35.03 | 64.598 |
| MULTI_ROUTER_ROUTER_REQREP | 65536 | 78,300 | 0 | 78,300 | 0.00 | 2.977 |

진단 원본 목록: [node-pass1c-final-timeouts-cells.json](reports/node-pass1c-final-timeouts-cells.json). Timeout 분모는 해당 run의 모든 REQUEST completion 결과 수다.

## 변경 파일과 규칙

- `bindings/node/native/src/addon_core.cc:689`: multipart가 공유하는 immutable 라우팅 properties를 한 번 생성한다.
- `bindings/node/src/zlink/runtime/sockets/socket_operations.ts:192`: 내부 소유 routing Buffer에 기존 `routingIdFromOwnedBuffer`를 사용한다. public factory의 복사 계약은 바꾸지 않는다.
- `bindings/node/src/zlink/contracts/messaging/received.ts:16`: ReplyToken의 private field와 WeakMap에 중복 저장하던 owner/value를 private field 하나로 통합한다. private brand로 위조 token을 거부한다.
- `bindings/node/tests/routed_receive_ownership.test.ts`: 두 peer 사이 Received 재사용, parts collection identity, 보관한 Buffer, 지연 reply의 원래 route/token, 위조 token 거부를 검증한다. 생성된 dist-tools test도 Node 범위 안에 있다.

**러너 수정: 없음.** 별도 러너 버그를 고친 효과를 library 효과와 합산하지 않았다. 기존 drain/direct reply·wrapper 재사용이 이미 요구 의미를 구현하고 있었으므로 이를 다시 구현하지 않았다.

대안 비교: native frame의 multipart 확장은 복사를 줄일 수 있지만 아래 기존 spec 불일치를 확대하므로 채택하지 않았다. 현재 변경은 managed Buffer 경로와 public 서명을 유지하고 중복 상태·생성만 줄인다.

**수정 전/후 규칙 수:** ReplyToken owner/value 저장 위치 2→1; 2-part 라우팅 properties 소유 객체 2→1; 내부 reply가 보유하는 routing byte storage 2→1. 수신 진행 제어점 1→1, 새 timer/spin/pool/in-flight cap 0→0.

- **소유 계층:** Node binding의 수신 객체 materialization, immutable routing metadata와 ReplyToken capability. Core의 readiness·routing·retry·completion 정책은 재구현하지 않는다.
- **Spec 조항:** `bindings/doc/spec/node/README.en.md:732–750` Receive and Subscribe Shape, `:866–868` ReplyToken private construction/equality, `:931–937` captured reply target, `:970–975` token owner rejection; perf 정책 `doc/perf/PERF_MULTI_TEST_POLICY.md` §1.1.
- **교차언어 대조:** C 서버도 ready socket을 drained까지 동기 수신하고 즉시 reply한다. Node 변경은 JS/N-API metadata·capability 비용에 한정된다. Framework runtime 수정은 없다.
- **변경 분류:** B — 기존 중복 생성·저장 제거. 새 계약이나 상위 계층 우회가 아니다.
- **Spec gap:** 채택 변경에는 없음. 기존 Node spec `README.en.md:738–741`은 routed part도 즉시 JS-owned Buffer이며 data read에 추가 native 호출이 없다고 기술하지만, 기존 `addon_core.cc:710` 부근의 single-part native-frame 분기는 다르다. 이 기존 불일치를 보고하며 이번 변경으로 확대하거나 보호 문서를 수정하지 않았다.

## Gate와 측정 조건

| 항목 | 최종 결과 |
|---|---|
| npm run build / native rebuild | PASS, JOBS≤3, Node 경로에서만 실행 |
| npm test | 132 tests / 28 files PASS |
| samples | 7/7 PASS |
| 관련 테스트 5회 | 23 tests ×5 PASS |
| 공개·내부 생성 d.ts | 87/87 byte-identical |
| git diff --check | PASS |
| 공식 after | 20/20 complete, RESULT 100/100, fail 0 |
| 작은 REQREP 수 ms latency | FAIL |

최종 측정에서 관측한 최대 1분 load는 **2.260**이며, 실제 Core mapping **129개**가 같은 고정 hash였다.

최종 gate 로그는 `reports/node-pass1c-final-full-gate.log`, `node-pass1c-final-related-five.log`다. 고정 Core의 actual loaded path와 hash는 각 `*-core-maps/` 및 final matched 디렉터리에 남겼다. raw runner META가 공유 Core 경로를 출력하더라도 실제 로딩된 라이브러리는 이 mapping으로 검증한 고정 파일이다.

최초 전체-grid run은 load>3으로 중단했으며 결과를 제외했다. 재측정은 셀 시작 load≤1.8, 실행 중 10초 간격 및 종료 시 load≤3을 확인한다. `--reuse-build`로 셀 안의 재빌드를 피하고 셀 사이에만 대기했다. 다른 현재 측정 프로세스와 겹치지 않도록 검사했다. 19시간 남아 있던 legacy 0.15.1 프로세스는 payload I/O가 변하지 않는 유휴 상태를 확인하고 각 셀마다 재확인했으며 종료시키지 않았다.

공유 Core는 16:51:35에 외부 교체됐다. 이후 고정 파일을 만들고 gate·20셀·동일 조건 계측·timeout 진단을 다시 실행했다. 교체 전의 20셀 및 진단은 보존하되 위 최종 표에 섞지 않았다. 초기 Core interposer의 RTLD_NEXT 해석 실패로 CLIENT_READY timeout이 난 진단도 보존·제외했다. 최종 기능 실패는 없다.

작업 트리 detached, pass1·1b 미커밋 변경과 기존 untracked 파일을 보존했다. commit/push/stash/checkout/reset 없음. Core configure/build/clean 없음. core/build·core/build-dev symlink 보존. 수정 소스는 bindings/node 범위뿐이다.
