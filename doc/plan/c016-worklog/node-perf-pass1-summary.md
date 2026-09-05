# Node binding hot-path pass 1

## 결과

- 구현·필수 gate·공식 after 완료. **목표 60%는 네 패턴 모두 미달**이다. DD는 C 대비 평균 27.56%에서 41.20%로 올라 최소 35%를 충족했다. REQREP 병목은 해결하지 못했다. Pass 2 전이므로 pattern 최종 판정을 닫지 않는다.
- 작업 트리 `/home/hep7hep7/project/zlink-wt-node-perf`, detached HEAD 유지. 변경은 `bindings/node/**`뿐이다. commit/push/reset/checkout/stash, Core configure/build/clean, spec/doc/다른 binding 변경 없음. 기존 `core/build`·`core/build-dev` symlink 보존.
- 공식 after는 **한 번**, tcp / 100 clients / 64,256,1024,4096,65536B / 5초 / 1 run / application parts 2 / I/O threads 4/4. **20/20 complete, RESULT 100/100, fail 0**.
- `/tmp/claude-1000/-home-hep7hep7-project-zlink/816a25a1-989b-4b8e-bf1c-f5ff8b96984a/scratchpad/measure_chain.done` 존재 확인 후 2026-09-05 **14:30:40 KST**, 1분 load **0.5718 ≤ 3**에서 시작했다. 그전에는 분석·빌드·단위 테스트·1초 진단만 실행했다. 공식 측정 중 다른 benchmark/빌드/test를 실행하지 않았다. JOBS=3.
- Core `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`, SHA-256 `a2069049d5c35e74b56ba3bc95183302f9ff1950d590340ccacb1e834256800e`. 공식 측정 전후 hash 동일.
- after 원본: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260905_143333.txt`. 사본: `reports/perf_node_multi_linux_20260905_143333.txt`.

| Pattern | Before/C 평균 | After/C 평균 | After latency/C 평균 | 판정 |
|---|---:|---:|---:|---|
| DD | 27.56% | 41.20% | 843.32× | 최소 35% 충족, 목표 60% 미달 |
| DR REQREP | 18.73% | 20.31% | 42.55× | 목표 60% 미달 |
| RR REQREP | 17.92% | 18.05% | 52.34× | 목표 60% 미달 |
| PUBSUB | 28.61% | 31.03% | 0.85× | 목표 60% 미달 |

비율 평균은 size별 비율의 산술평균이다. DD latency는 queue 깊이에 크게 좌우되는 참고값이며, 요청에서 제시한 기준대로 통과 판정에서 제외한다. **DD latency 악화와 RR 4096B −57.9%, RR 1024B −28.3%, DR 64B −21.9%를 숨기거나 무효화하지 않았다.** 단일 run이며 공식 after를 유리한 값이 나올 때까지 반복하지 않았다.

## 비용 위치와 가이드 §2 판정

`node --cpu-prof`, `--heap-prof`, V8 `--prof`와 별도 preload의 native call counter·GC 포함 HeapProfiler sampling을 사용했다. `perf` 실행 파일은 설치돼 있지 않다. 실제 Node DD/DR 러너, tcp/64B/8 clients/1초로 짧게 계측했다. 계측 run은 공식 throughput 판정에 사용하지 않는다.

| 위치 | 근거 | 판정·변경 |
|---|---|---|
| DD native submit | 최초 CPU profile: client `CompletionOwner.submitSend` native 호출 구간 705.1ms/1.15초. SEND 147,742건 모두 즉시 OK, runtime watch 생성 0회 | §2.1. 이미 SEND entry/Promise/map은 만들지 않는다. 남은 native 결과 객체와 Buffer 변환 배열·진단 문자열 할당 제거 |
| DD native receive | 최초 CPU profile: server `recv` 699.8ms/1.29초. `addon_core.cc` before 2666의 vector는 2 parts에 capacity 1→2로 성장 | §2.4. DD/SUB도 기존 `small_msg_storage_t`로 통일. 8 parts 이하는 staging heap allocation 0회, 큰 multipart는 기존 spill 규칙 사용 |
| Core wake | V8 DD client native ticks 중 `epoll_ctl` 172/1107. LD_PRELOAD stack + addr2line: `zlink_send_part` → `send_direct_with_retry` → pipe flush → mailbox send → Asio `post_immediate_completion` | Core가 소유한 전송 진행 비용. binding의 성공 SEND poller 생성으로 오인하지 않았다. Core 수정·우회 없음 |
| DR native 경계 | 최초 CPU profile: server recv 463.0ms + reply 415.1ms; client submitRequest 336.1ms + drain 160.5ms. 서버 poll wait 300.3ms는 처리 CPU와 분리 | binding/native 경계와 Core 진행이 우세. CPU sampler의 native 호출 프레임은 Core 비용도 포함하므로 전부 N-API 변환 비용으로 단정하지 않는다 |
| DR completion wake | 최종 before 진단: 요청 52,216건, callback 343,807회, completion pull 396,023회. 아래 계수 표 참조 | `completion_owner.ts:453,659,668`의 단일 owner가 실제 FD wake 뒤 NO_DATA까지 drain. REQUEST당 JS timer와 uv_async는 없음. 빈 FD wake 비용은 남음. 정확한 ready source를 제공하는 pump 설계는 별도 검토 필요 |
| REQREP 러너 | `perf_multi_socket_reqrep.ts:95–115`: 모든 socket에 요청 제출 후 setImmediate, 개별 요청 await는 독립 task 내부 | Java before의 socket별 settlement 직렬 대기와 다름. scheduler/drain/fairness 변경 없음. runner 버그 수정 항목 없음 |
| Backpressure | 기존 token/context/RID registry, retry snapshot과 오류 분류 유지 | §2.2/2.5. 성공 경로 최적화가 실제 거절 뒤 Buffer snapshot, Message 소비, terminal error 전달을 바꾸지 않음 |

### 메시지·요청당 비용: C 대조

C 열은 paired C 러너의 public part 호출·복사 경로를 코드로 계수한 값이다. Node N-API·wake 값은 최종 별도 계측값이다. **Core 내부 전체 malloc 수나 전체 CPU를 C 대비 추정한 표가 아니다.** V8 managed allocation은 C에는 존재하지 않으며 native Core allocation은 양쪽 모두 별도 비용이다.

| 항목 (64B + empty tail) | C | Node before | Node after |
|---|---:|---:|---:|
| JS→N-API DD send 호출/전송 | 0 | 1 | 1 |
| JS→N-API DD recv 호출/수신 (NO_DATA 포함) | 0 | 1.00727 | 1.00041 |
| JS→N-API DR client submit+completion pull/요청 | 0 | 8.58432 | 9.38559 |
| JS→N-API DR server recv+reply/수신 | 0 | 2.01437 | 2.00251 |
| DD Core send_part / recv_part (정상 2-part) | 2 / 2 | 2 / 2 | 2 / 2 |
| DR Core request_part / router_recv_part / reply_part | 2 / 2 / 2 | 2 / 2 / 2 | 2 / 2 / 2 |
| DD 2-part 수신 header staging heap 성장 횟수 | 0 (stack) | 2 (vector 1→2) | 0 (inline) |
| 즉시 SEND용 새 completion entry·Promise·map | 0 | 0 | 0 |
| 즉시 SEND native 결과 객체/전송 | 0 | 1 | 0 (함수 수명의 immutable 1개) |
| Buffer-only 2-part 변환 배열/전송 | 0 | 1 | 0 |
| 정상 payload용 parts[index] 진단 문자열/전송 | 0 | 2 | 0 |
| DR uv_poll callback/요청 | 해당 없음 | 6.58432 | 7.38559 |
| DR client setImmediate/요청 (8 clients 진단) | 해당 없음 | 0.125 | 0.125 |
| binding uv_async/요청 | 해당 없음 | 0 | 0 |
| DD payload 복사 bytes/성공 메시지 (sender+receiver) | 64 | 128 | 128 |
| DR payload 복사 bytes/성공 왕복 (client+server) | 64 | 256 | 256 |

copy 표는 payload 본문만 포함한다. RID/header·metric stamping·backpressure snapshot 및 Core wire framing은 제외한다. C 근거: `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:168`, `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:257,813`. Node `init_msg_from_bytes`와 `create_received_message_buffer`의 복사 규칙은 그대로다. Timeout completion은 reply body가 없으므로 실측 `receiveCopyBytes / admittedRequests`가 64보다 작다. 성공 왕복 표에서 이를 복사 절감으로 계산하지 않았다.

### GC 포함 V8 allocation sampling

32KiB sampling, collected-by-minor/major-GC 포함. 아래는 runner와 binding stack의 sampled bytes 합/operation이며 startup·기타는 제외한다. 최적화/inlining에 따라 allocation stack이 caller 쪽으로 이동하므로 수신의 binding/runner 세부 비중을 독립적인 확정값으로 사용하지 않았다. Native malloc과 Core payload storage는 이 값에 포함되지 않는다. Counter wrapper는 hot call에서 배열·결과 객체를 새로 만들지 않는 고정 인자 함수다.

| 경로 | before / after 계수 | Before B/op | After B/op | 변화 |
|---|---:|---:|---:|---:|
| DD 송신 | 195,764 / 233,865 | 2001.74 | 1702.50 | -14.9% |
| DD 수신 | 195,764 / 233,865 | 1128.93 | 1154.59 | +2.3% |
| DR 요청 | 52,216 / 54,312 | 4062.00 | 3880.31 | -4.5% |
| DR 응답 | 51,495 / 54,185 | 2395.66 | 2445.39 | +2.1% |

DD 송신의 binding stack만 보면 249.42→51.04 B/op다. 수신 managed allocation은 감소했다고 주장하지 않는다. 수신 변경의 근거는 native vector 성장 제거다. DR client에는 요청 entry/Promise와 runner의 task·pending Set·payload copy가 남는다.

## 채택·기각 및 소유권

- 채택: `message_conversion.ts:13–35`에서 같은 Buffer 리스트는 재구성하지 않고 실제 string/Uint8Array/Message 변환이 필요할 때만 prefix를 복사한다. 오류 문자열은 오류 시점에 생성한다. 리스트의 payload bytes를 빌려 보내는 변경이 아니며 native는 호출 안에서 계속 복사한다.
- 채택: `addon_exports.cc:119–138`, `addon_core.cc:2075–2124`에서 SEND native 함수가 환경별 immutable OK 결과 하나를 소유한다. 함수 finalizer가 reference를 해제한다. 전역 V8 handle·public wrapper pool·토큰 재사용 없음. 공개 SEND 결과는 계속 `Promise<void>`다.
- 채택: `addon_core.cc`의 DD/SUB 수신을 기존 inline/spill storage로 통일하고, `addon_message_parts.h`의 사용처 없어진 vector append/collect 구현을 삭제했다.
- 기각: multipart ROUTER 수신을 native frame으로 넘기는 후보. 본문 복사는 줄지만 recv/data/close에 native 호출·외부 handle finalizer가 늘었다. 64B profile 25.3k→15.5k, 별도 계수 진단 30.9k→13.0k로 하락했다. 큰 payload mini도 변동이 커 독립 이득을 확정하지 못했다. Node receive의 managed Buffer 경계와도 대조해 **최종 코드에서는 전부 제거**했다. 이는 가이드 §4의 TSFN STREAM pool을 다시 시도한 것이 아니다.
- 미채택: public wrapper/Promise pool, Buffer borrow, runner in-flight cap, timeout/retry 증가, zero-timeout spin, drain scheduler 변경. empty FD wake를 임의 sleep이나 poll 재예약으로 숨기지 않는다.
- **수정 전/후 규칙 수:** receive header staging 구현 2종(vector/inline)→1종(inline+기존 spill); completion drain owner 1→1. 동일 staging 결정을 두 곳에 유지하지 않는다.
- **소유 계층:** binding runtime의 Buffer 변환과 N-API 임시 표현·자원 수명. 전송/admission/WRITABLE 발생은 Core, drain/retry 판단은 기존 `CompletionOwner`.
- **spec 조항:** Node spec `bindings/doc/spec/node/README.ko.md:520–528` Buffer 복사/소유권, `:680–685` 수신 Buffer 수명; 공통 `async-execution-model.ko.md:63–84` 단일 drain owner; Core socket Part send의 OK/ID 0와 BACKPRESSURED token 계약.
- **교차언어 대조:** C++/.NET/Java가 사용하는 inline/scratch native staging과 같은 수명 경계다. .NET pass 1의 즉시 SEND 등록 제거는 Node에 이미 적용돼 있어 중복 구현하지 않았다. Java pass 1의 socket별 settlement 대기는 Node 러너에 없으며, Java pass 1b의 Context pump와 Node FD watcher의 진행 방식 차이는 남아 있다.
- **변경 분류:** B — 기존 binding hot-path의 불필요한 할당·중복 storage 구현 정리. 공개 계약 적응이나 Core 버그 보상 아님.
- **spec gap:** 채택 변경에는 없음. 새 public API·새 completion 규칙을 요구하지 않는다.

## 공식 before/after 상세

K 단위는 DD/PUBSUB Kmsg/s, REQREP Kops/s. Latency는 ms이며 마지막 열은 after/C mean latency 비율이다. Before/C 원본 8개도 `reports/`에 복사했다.

| Pattern | B | Before K/s | After K/s | C K/s | After/Before | After/C | Lat.Mean before → after ms | Lat/C |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| DD | 64 | 160.702 | 290.708 | 1017.379 | +80.9% | 28.57% | 245.138 → 1023.060 | 2932.50× |
| DD | 256 | 166.708 | 260.249 | 955.021 | +56.1% | 27.25% | 178.692 → 1287.034 | 631.43× |
| DD | 1024 | 169.680 | 236.411 | 832.374 | +39.3% | 28.40% | 506.008 → 1407.398 | 632.09× |
| DD | 4096 | 125.971 | 148.269 | 315.562 | +17.7% | 46.99% | 1356.764 → 1449.510 | 2.49× |
| DD | 65536 | 26.843 | 45.400 | 60.698 | +69.1% | 74.80% | 9.057 → 366.656 | 18.07× |
| DR REQREP | 64 | 10.988 | 8.582 | 145.258 | -21.9% | 5.91% | 57.443 → 57.439 | 49.17× |
| DR REQREP | 256 | 10.067 | 9.141 | 163.473 | -9.2% | 5.59% | 52.196 → 52.186 | 54.70× |
| DR REQREP | 1024 | 8.260 | 7.925 | 155.282 | -4.1% | 5.10% | 57.905 → 59.302 | 61.64× |
| DR REQREP | 4096 | 11.924 | 13.913 | 122.937 | +16.7% | 11.32% | 55.760 → 56.297 | 46.04× |
| DR REQREP | 65536 | 14.408 | 16.334 | 22.189 | +13.4% | 73.62% | 3.561 → 2.973 | 1.23× |
| RR REQREP | 64 | 9.709 | 9.399 | 176.436 | -3.2% | 5.33% | 61.153 → 61.790 | 74.07× |
| RR REQREP | 256 | 7.522 | 7.388 | 144.377 | -1.8% | 5.12% | 58.966 → 55.168 | 71.01× |
| RR REQREP | 1024 | 8.387 | 6.014 | 139.640 | -28.3% | 4.31% | 60.077 → 56.354 | 65.38× |
| RR REQREP | 4096 | 14.833 | 6.248 | 113.689 | -57.9% | 5.50% | 63.247 → 56.991 | 49.48× |
| RR REQREP | 65536 | 12.871 | 15.066 | 21.515 | +17.1% | 70.03% | 4.875 → 4.196 | 1.76× |
| PUBSUB | 64 | 111.746 | 158.159 | 588.072 | +41.5% | 26.89% | 1378.658 → 1494.076 | 0.99× |
| PUBSUB | 256 | 164.834 | 172.446 | 777.996 | +4.6% | 22.17% | 242.271 → 324.433 | 0.16× |
| PUBSUB | 1024 | 170.749 | 170.656 | 849.159 | -0.1% | 20.10% | 60.332 → 61.940 | 0.06× |
| PUBSUB | 4096 | 146.234 | 148.604 | 646.535 | +1.6% | 22.98% | 24.714 → 35.077 | 0.10× |
| PUBSUB | 65536 | 41.121 | 43.072 | 68.383 | +4.7% | 62.99% | 471.857 → 406.249 | 2.92× |

Before Node: `perf_node_multi_linux_20260905_{135524,135649,135755,135859}_p1node.txt`.
Paired C: `perf_c_multi_linux_20260905_{135418,135525,135650,135755}_p1node.txt`.
After: `perf_node_multi_linux_20260905_143333.txt`. C는 사용자가 지정한 paired baseline이며 새 공식 C 재측정은 하지 않았다. before와 after 시작 load가 다르고 1-run이므로 전체 차이를 개별 변경의 독립 효과로 분해하지 않는다.

## Gate와 남은 실패

| Gate | 결과 | 근거 |
|---|---|---|
| TS `npm run build`, addon rebuild | PASS, JOBS=3 | `node-full-gate.log`의 npm pretest |
| `npm test` 전체 dist-tools | **126 tests / 26 files PASS** | `node-full-gate.log` |
| samples | **7/7 PASS** | 같은 log 끝 sample summary |
| 필수 `typecheck`, `source_layout` | PASS | npm pretest, full test |
| 최종 변경 관련 테스트 5회 | **24 tests × 5 PASS** | `node-related-final-gate.log`; multipart_hot_path, send_completion_boundary, send_completion_operation_path, request_admission, xpub_xsub |
| public .d.ts | **31파일 byte-identical** | `reports/node-public-signatures.txt` |
| `git diff --check` | PASS | 최종 diff 검증 |
| 공식 after | **20/20 complete**, 실패 0 | after report |
| 추가 `typecheck:src-review` | 기존 실패 **TS6133** | `src/zlink/contracts/messaging/topic_message.ts:18`의 `_reusableSinglePartSlots`; HEAD에도 존재하며 이번 변경 없음 |

최초 빌드는 worktree에 node_modules가 없어 실패했고 `npm ci --ignore-scripts --no-audit --no-fund` 후 정상화했다. package-lock 변경 없음.

추가 테스트 초안의 17×약64KiB REQUEST는 중간 MORE에서 적용 HWM(1MiB)을 넘었다. 변경 전에도 BACKPRESSURED였으며 Core auto-HWM spec의 “중간 MORE에 oversize 예외 미적용”과 일치한다. 이 초안의 수락 가정은 잘못됐다. 17-part·64KiB body·내용/소유권 assertion을 유지하며 유효한 총량으로 교정했다. 진단 원본은 `reports/node-oversize-request-repro.test.ts`, baseline 실패는 `node-newtest-baseline.log`에 보존한다. 기존 테스트의 assertion, timeout, HWM 설정은 바꾸지 않았다. 잘못된 초안은 Core spec gap으로 보고하지 않는다.

## 변경 파일

- `bindings/node/src/zlink/runtime/buffers/message_conversion.ts`
- `bindings/node/native/src/addon_core.cc`
- `bindings/node/native/src/addon_exports.cc`
- `bindings/node/native/src/addon_message_parts.h`
- `bindings/node/tests/multipart_hot_path.test.ts`
- `bindings/node/dist-tools/tests/multipart_hot_path.test.js` — 저장소가 추적하는 dist-tools 형식의 생성 테스트.

## BLOCKERS

- 필수 기능 gate·공식 report 생성의 blocker는 없다.
- **성능 목표는 미달이며 REQREP의 빈 FD wake·native/Core 진행 비용이 남는다. RR 4096B 등 회귀 셀의 원인을 이 pass에서 확정하지 못했다.** Pass 2 검토 전 전체 성능 승인·pattern 종료 판정은 하지 않는다.
- 추가 src-review gate는 위 기존 TS6133 때문에 실패한다. 요청 범위 밖의 contracts 필드를 제거하거나 unused 검사를 완화하지 않았다.

## 진단 자료

- `reports/node-profile-before/`: 최초 `--cpu-prof`·`--heap-prof` 원본.
- `reports/node-count-before/dd-client-prof.txt`, `reports/epoll-*.txt`: V8 native ticks와 Core epoll 호출 스택.
- `reports/node-cost-before/`, `reports/node-cost-after/`, `reports/node-final-cost-comparison.json`: 최종 GC 포함 allocation·native/wake/copy 계수.
- `reports/node-official-comparison.json`: 20개 공식 셀의 원시 비교 값.
- `reports/node-profile-candidate/`, `node-profile-candidate.log`, `reports/node-count-final/`: **기각한 native-frame 후보** 자료이며 최종 after가 아님.
- 진단 preload와 baseline 사본은 `reports/`에만 있으며 최종 package source에는 임시 로깅·계수 hook을 남기지 않았다.

EXIT:0
