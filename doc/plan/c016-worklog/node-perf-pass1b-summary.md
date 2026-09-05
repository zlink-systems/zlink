# Node completion progress pass 1b

## 결과

**기능 수정·gate·최종 20셀 report는 완료했지만, 수ms REQREP latency 목표와 공유 POLLCOMPLETION 구조 개선은 달성하지 못했다. 성능 승인 BLOCKED, EXIT:2.**

최종 코드에는 기존 socket별 단일 `CompletionOwner`와 FD watcher를 유지하고, libuv→JavaScript 진입을 정식 N-API async resource/`napi_make_callback()` 경계로 고친 변경만 남겼다. 같은 Core에서 별도 계측한 pull→Promise continuation은 **0.588→0.012ms**로 줄었다. 전체 요청 수명은 **129.827→130.204ms**로 유지됐다. 서버 TCP recv→Core public recv 대기가 각각 **127.757/128.581ms**로 대부분을 차지했다. 이 결과를 57ms의 주원인이 completion timer였다는 증거로 해석하지 않는다.

최종 공식 after는 **20/20 complete, RESULT 100/100, fail0**. Before는 pass1의 공식 after이며 C는 기존 paired baseline이다. 비율은 size별 비율의 산술평균이다.

| Pattern | Before/C | Final After/C |
|---|---:|---:|
| DD | 41.20% | 25.34% |
| DR REQREP | 20.31% | 11.02% |
| RR REQREP | 18.05% | 18.91% |
| PUBSUB | 31.03% | 30.53% |

**DD 41.20→25.34%, DR 20.31→11.02%와 개별 회귀 셀을 그대로 남긴다.** Core가 작업 중 외부 교체됐고 공식 값은 1-run이므로 역사적 차이를 Node 수정만의 독립 효과로 주장하지 않는다. 후속 단독 진단으로 공식 셀을 교체하거나 무효화하지 않았다.

- 최종 report: `reports/perf_node_multi_linux_20260905_161133.txt`.
- 기각 Context pump report: `reports/perf_node_multi_linux_20260905_153608.txt`.
- 작업 트리 detached 유지. 최초 pass1 미커밋 변경과 untracked 파일 보존. commit/push/checkout/reset/stash 없음. 수정 소스 범위는 `bindings/node/**`뿐이며 러너·spec/doc·Core·다른 binding 변경 없음. `core/build`, `core/build-dev` symlink 보존; Core configure/build/clean 없음.

## REQUEST 수명과 빈 wake 진단

### 관측 방법과 의미

동일한 실제 DR/tcp/64B/100 clients/5초 러너에 외부 JS preload와 C ABI interposer를 붙였다. `CLOCK_MONOTONIC`과 Node hrtime을 사용해 application metric header의 seq로 프로세스 경계를 연결했다. 97간격으로 샘플링해 100 socket 전체를 순환한다. Core `request_part` FINAL, TCP `recv`, `router_recv_part`, `reply_part` FINAL, `completion_recv` 반환과 JS callback/capture/Promise continuation을 관측했다. Observer·버퍼·임시 계수는 package source에 남기지 않았다.

아래는 **같은 성공 요청에서 모든 구간이 관측된** baseline974건/수정1019건의 평균이다. Timeout 요청의 긴 서버 체류 시간을 성공 요청 평균에 섞지 않았다. 계측 run의 처리량은 공식 after가 아니다. Observer 오버헤드와 실행 조건이 있으므로 역사적 57ms run 자체의 내부 시각을 소급 측정했다고 주장하지 않는다. 다만 유사한 수십ms latency가 재현됐고 주 지연 구간이 확인됐다.

| 관측 구간 | Baseline ms | 최종 수정 ms |
|---|---:|---:|
| JS submit 진입→Core FINAL admission | 0.005 | 0.005 |
| Core admission→서버 TCP recv | 0.116 | 0.125 |
| 서버 TCP recv→Core router_recv_part(첫 body part) | 127.757 | 128.581 |
| Core router_recv_part→Core reply FINAL | 0.018 | 0.020 |
| Core reply FINAL→클라이언트 TCP recv | 0.161 | 0.188 |
| 클라이언트 TCP recv→Core completion_recv 반환 | 1.183 | 1.273 |
| Core completion_recv 반환→Promise continuation | 0.588 | 0.012 |
| 전체 관측 왕복 | 129.827 | 130.204 |

개별 baseline 수명 예시(seq **78473**, 성공 표본 왕복 중앙값에 가까운 요청):

`socketSubmitRequest +0.000ms → coreAdmit +0.005ms → wire +0.285ms → coreRecv +145.676ms → coreReply +145.697ms → wire +145.748ms → corePull +145.822ms → promise +146.108ms`

러너의 REQREP Lat.Mean은 RTT/2다(`perf/common/perf_measurement.ts:286`). 따라서 관측 왕복 약130ms는 보고 latency 약65ms에 해당한다. 측정 의미와 실패/timeout 처리 방식은 변경하지 않았다.

Core queue enqueue와 kernel FD readiness 전이 자체는 공개 observer로 직접 계측하지 못했다. **client TCP recv→Core completion pull 전체가 약1.2ms**이므로 그 안의 queue 진행·FD 대기·Node 진입을 합쳐도 관측된 주 지연을 설명하지 못한다. Runtime 경로에 REQUEST용 `uv_async`, 50ms timer, setImmediate 재시도는 없었다. Server loop의 50ms poll timeout(`perf_multi_dealer_router_server.ts:103`)과 구별해야 한다.

서버 TCP 수신 뒤 public dequeue까지의 지연은 서버 입력/처리 backlog 구간이다. 입력 큐 내부 enqueue 시점과 세부 처리 비용까지 분해한 것은 아니다. Client는 100 socket에 계속 submit하고 `setImmediate`로 넘긴다(`perf_multi_socket_reqrep.ts:95–115`); pending Set은 in-flight cap이 아니다. C도 TCP에서는 같은 client-count 단위로 submit 후 completion을 진행하며 outstanding을 1개로 제한하지 않는다(`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:513–526,574–603`). **C에만 1-request cap이 있다고 설명하지 않는다.**

### 빈 FD callback

| 항목 | Baseline | 최종 수정 |
|---|---:|---:|
| REQUEST submit 호출 | 223,700 | 199,400 |
| OK completion | 94,564 | 99,059 |
| TimedOut completion | 129,136 | 100,341 |
| runtime FD callback | 262,908 | 244,712 |
| completion을 0건 처리한 FD callback | 159,500 | 136,236 |
| completion pull (NO_DATA 포함) | 486,608 | 444,112 |
| callback / admitted request | 1.175 | 1.227 |
| empty callback / admitted request | 0.713 | 0.683 |
| pull / admitted request | 2.175 | 2.227 |

Core mailbox FD인 `ZLINK_OPT_FD`의 readable wake를 Node runtime이 completion callback으로 사용한다(`addon_core.cc:2360` 부근). 이 FD는 completion record 존재를 보장하는 `POLLCOMPLETION` event가 아니다. Callback은 그대로 `CompletionOwner.runtimeWake`→`drain`을 호출한다(`completion_owner.ts:453,668`), Core는 command progress 후 비어 있는 queue에 NO_DATA를 반환할 수 있다. Raw mailbox readiness를 completion readiness와 동일시한 데서 불필요한 callback이 생긴다. 최종 수정은 이 원인을 제거하지 않았다.

모든 정상 drain도 끝에서 NO_DATA를 한 번 읽으므로 **NO_DATA 횟수를 전부 빈 wake로 세지 않았다.** 표의 empty callback은 실제 callback 전후 처리 completion 수가 모두 0인 경우다. 최초 배경의 요청당 약7회는8-client/짧은 별도 계측이며, 위100-client 값과 같은 수치로 취급하지 않는다.

## 설계 비교와 최종 변경

| 대안 | 제어와 상태 | 판정 |
|---|---|---|
| 기존 TS owner/drain 유지 | Public poller와 runtime이 이미 같은 `CompletionOwner.drain` 사용 | 유지. 여러 TS drain 구현이 있다는 전제는 맞지 않음 |
| raw FD에 임의 재조회·재예약 추가 | mailbox notification 해제/재무장 지식을 binding에 추가해야 함 | 미채택. 새 timer/spin/retry 규칙으로 보상하지 않음 |
| 환경당 공유 Core poller | 서로 다른 Context의 native wait 오류를 한 pump가 받음 | 기각. 종료된 Context의 오류가 다음 Context를 오염시키는 실제 단위 실패 |
| Context당 공유 Core poller·worker·delivery FD | Core POLLCOMPLETION wait 후 기존 TS drain; public handover와 wait 직렬화 | 기각. 기능 gate와20셀 report는 통과했지만 추가 delivery hop과 심한64KiB 회귀. source/binary/log 보존 |
| **기존 watcher의 정식 Node async callback scope** | 기존 callback reference를 async resource 수명에도 사용; Node가 callback/microtask scope 소유 | **최종 코드**. 전체 기능 gate 통과, pull→Promise 지연 감소. 전체 성능 목표 달성은 아님 |

공유 pump의64KiB 공식 결과는 DR807.6/RR584.8ops/s였다. Core 교체 confound를 확인한 뒤 **같은 현재 Core + pass1 addon** 단독5초 진단을 실행해 DR16,435/RR15,240.8ops/s를 얻었다. 따라서 pump의 큰 격차는 Core 교체만으로 설명되지 않는다. Shared pump의 모든 thread/poller/control channel/연결 registry 코드는 최종 tree에서 제거했다.

최종 DR64KiB 공식 값은7,938.6ops/s,6.357ms다. 이 회귀를 확인하려고 같은 Core에서 별도 단독5초 진단을 했고 **16,697ops/s,2.310ms**가 나왔다. Matched baseline 단독 값은16,435ops/s,2.883ms다. 큰 격차가 이 단독 조건에서 재현되지는 않았지만, **공식 full-suite 셀을 교체하지 않으며 run/조건별 차이 원인은 미해결**이다.

최종 파일:

- `bindings/node/native/src/addon_core.cc:2266,2284,2339,2407`: watcher async context 생성·해제, 기존 callback reference 재사용, native→JS 진입에 `napi_make_callback` 사용.
- `bindings/node/tests/completion_progress.test.ts`: 무관한 이벤트 없는 연속 요청, public/runtime owner 이전·DATA 보존, 독립 Context 종료, async callback context, public wait 전 settlement 금지와 close 오류.
- `bindings/node/dist-tools/tests/completion_progress.test.js`: 생성 테스트.

N-API는 async native→JS 진입에 `napi_make_callback`을 제공하며, 유효한 async context 없이 호출하면 async hooks 문맥이 손실될 수 있다고 규정한다. 이를 따라 `napi_async_init`과 기존 callback reference로 수명을 보장했다. [Node-API 공식 계약](https://nodejs.org/api/n-api.html#napi_make_callback).

Baseline addon으로 새 context 회귀를 실행하면 `AsyncLocalStorage.getStore()`가 `undefined`라 실패한다. 수정 후에는 `request-owner`를 보존한다. 기존 public `Promise`/ownership/error/declaration은 유지한다. 새 timer·spin·pool·retry budget·runner scheduler·두 번째 poller·새 요청 registry는 없다.

- **소유 계층:** binding native watcher는 Node async callback resource 수명, 기존 socket CompletionOwner는 단일 drain/correlation/settlement. Core의 reply/command/retry readiness 결정을 재구현하지 않음.
- **Spec 조항:** `bindings/doc/spec/async-execution-model.ko.md:63–84` §4 단일 drain/public handover; Node README:780–792 pull/Promise/progress; Core 05-polling §4–5 readiness와 wait 직렬화. Native callback 진입은 위 Node-API 계약.
- **교차언어 대조:** Java Context pump와 .NET 공통 drain은 설계 비교에 사용했으나 Node 성능 개선 근거로 그대로 복사하지 않았다. 채택한 N-API callback scope는 libuv를 쓰는 Node 고유 경계이며 다른 언어 runtime 변경 없음.
- **변경 분류:** B — 기존 native async 진입 경계의 결함 수정. 주 latency 병목이나 전체 성능 목표를 해결한 변경으로 분류하지 않음.
- **수정 전/후 규칙 수:** TS completion drain owner/구현1→1; callback 진행은 일반 native 호출 뒤 별도 JS 진입에 기대던 경계에서 표준 Node async callback scope 하나로 통합. 추가 thread/poller/timer/retry/registry 정책0→0. Async resource의 strong reference는 기존 callback reference 하나를 재사용.
- **Spec gap:** 채택 변경에 없음. Core public API에는 completion 전용 poller FD를 libuv에 직접 넘기는 API가 없으며, 공개 Core poller를 쓰는 공유 worker 대안은 성능 기준을 충족하지 못했다. 보호 문서는 수정하지 않음.

## 공식 before/after 상세

DD/PUBSUB 단위는 Kmsg/s, REQREP는 Kops/s. Latency는 기존 ms 의미다. C와 pass1 before 원본은 기존 `node-official-comparison.json` 및 pass1 요약의 paired reports를 사용했다.

| Pattern | B | Before K/s | After K/s | C K/s | After/Before | After/C | Latency before→after ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| DD | 64 | 290.708 | 228.294 | 1017.379 | -21.5% | 22.44% | 1023.060→1215.052 |
| DD | 256 | 260.249 | 118.721 | 955.021 | -54.4% | 12.43% | 1287.034→1180.316 |
| DD | 1024 | 236.411 | 149.739 | 832.374 | -36.7% | 17.99% | 1407.398→1124.200 |
| DD | 4096 | 148.269 | 86.458 | 315.562 | -41.7% | 27.40% | 1449.510→2256.392 |
| DD | 65536 | 45.400 | 28.187 | 60.698 | -37.9% | 46.44% | 366.656→572.767 |
| DR REQREP | 64 | 8.582 | 6.322 | 145.258 | -26.3% | 4.35% | 57.439→67.793 |
| DR REQREP | 256 | 9.141 | 11.158 | 163.473 | +22.1% | 6.83% | 52.186→71.597 |
| DR REQREP | 1024 | 7.925 | 7.447 | 155.282 | -6.0% | 4.80% | 59.302→66.897 |
| DR REQREP | 4096 | 13.913 | 4.104 | 122.937 | -70.5% | 3.34% | 56.297→55.784 |
| DR REQREP | 65536 | 16.334 | 7.939 | 22.189 | -51.4% | 35.78% | 2.973→6.357 |
| RR REQREP | 64 | 9.399 | 15.357 | 176.436 | +63.4% | 8.70% | 61.790→63.749 |
| RR REQREP | 256 | 7.388 | 19.482 | 144.377 | +163.7% | 13.49% | 55.168→54.472 |
| RR REQREP | 1024 | 6.014 | 10.875 | 139.640 | +80.8% | 7.79% | 56.354→56.236 |
| RR REQREP | 4096 | 6.248 | 4.737 | 113.689 | -24.2% | 4.17% | 56.991→52.360 |
| RR REQREP | 65536 | 15.066 | 12.991 | 21.515 | -13.8% | 60.38% | 4.196→4.031 |
| PUBSUB | 64 | 158.159 | 158.314 | 588.072 | +0.1% | 26.92% | 1494.076→1498.833 |
| PUBSUB | 256 | 172.446 | 165.758 | 777.996 | -3.9% | 21.31% | 324.433→497.429 |
| PUBSUB | 1024 | 170.656 | 170.898 | 849.159 | +0.1% | 20.13% | 61.940→69.223 |
| PUBSUB | 4096 | 148.604 | 147.128 | 646.535 | -1.0% | 22.76% | 35.077→53.099 |
| PUBSUB | 65536 | 43.072 | 42.071 | 68.383 | -2.3% | 61.52% | 406.249→448.341 |

## Gate와 측정 조건

| Gate | 결과 | 근거 |
|---|---|---|
| `npm run build`, `./scripts/rebuild_native.sh`, JOBS3 | PASS | final callback build / final npm pretest |
| 최종 `npm test` | **131 tests,27 files PASS** | `node-pass1b-final-full-gate-recovered.log` |
| samples | **7/7 PASS** | 같은 final log 끝 |
| 관련 테스트5회 | **29 tests ×5 PASS** | `node-pass1b-final-related-five.log` |
| 공개/내부 생성 d.ts | **87파일 byte-identical** | `reports/node-pass1b-public-signatures.txt` |
| `git diff --check` | PASS | 최종 검증 |
| 최종 공식 after | **20/20 complete, RESULT100/100, fail0** | `perf_node_multi_linux_20260905_161133.txt` |
| 수ms REQREP 목표·성능 승인 | **FAIL / BLOCKED** | 주 지연 및 공식 회귀 셀 미해결 |

측정 완료 파일은15:24:45 확인했고 load0.67에서 첫 진단을 시작했다. 그전에는 분석·설계·구현·단위 테스트만 했다. Samples도 완료 파일 확인 이후 실행했다. 최종 matched trace 시작 load는2.976/2.887, same-Core baseline64KiB는1.920이다. 각 run 전후와 두 trace 사이의 Core hash를 비교했다. 최종 공식 시작은 progress log의16:08 부근이며 load≤3 guard를 통과했다. 최종 공식/별도 진단을 수행하는 동안 이 작업의 build/test를 병렬 실행하지 않았다.

외부 대규모 C++/LTO 빌드가 시작돼 load가150을 넘었을 때 추가 계측 직전 guard가 실행을 차단했다. 부하 중 최종 unit gate의 기존 동시 TCP/WSS SENDSEND가30초 제한으로 실패했다. Timeout/assertion을 바꾸지 않고 부하 회복 뒤 먼저 해당 파일10개 테스트를 통과시킨 다음 전체 gate를 다시 실행했다. **남은 기능 실패는0**이다. 높은 load에서의 실패 로그는 보존했다.

시작 Core hash는 `a2069049d5c35e74b56ba3bc95183302f9ff1950d590340ccacb1e834256800e`였다. 공유 library가 외부 작업으로 바뀌었으며 확인 당시 mtime은15:15:18이었다. 최종 trace·matched baseline64KiB·최종 공식 before/after Core는 모두:

`543e1089430176bf861f9ef8b7974941e3d785dee8d93bb8d4a39d62e1d08538`

최종 공식 전후 hash는 동일하다. 역사적 pass1/C와 Core hash가 다르다는 제한은 남는다. Core configure/build/clean이나 library 교체를 이 작업에서 수행하지 않았다.

## BLOCKERS

1. **수ms REQREP 목표 미달.** Completion 전달보다 서버 TCP 수신→Core public recv 구간의 backlog가 주 지연이다. 그 구간의 세부 병목과 full-suite 셀 변동은 미해결이다.
2. **공유 POLLCOMPLETION 구조 채택 실패, 빈 FD wake 잔존.** 공유 pump는 구현·검증·측정 후 기각했으며, 최종 변경은 Native callback scope로 제한됐다.
3. **성능 회귀/비교 제한.** DD·DR 평균 및 DR4096/64KiB 등의 공식 회귀를 해소하지 못했다. 외부 Core 교체·1-run·full-suite/단독 차이를 분리해 Node 변경의 전체 성능 개선을 확정하지 못한다.

진단 자료는 `reports/node-pass1b-final-trace-{before,after}/analysis.json`·native CSV·JS trace, observer 소스, baseline addon, 기각 pump source/binary/log, 최종20셀 comparison JSON에 보존했다. 진행 로그는 `node-perf-pass1b-progress.md`다.

EXIT:2
