# Node binding 0.17.0 wait-token port review

검토 대상은 `bindings/node/**`이며, Core는 기존 `core/build/lib/libzlink.so.0.17.0`만 사용했다. 공개 API 형태는 바꾸지 않았다.

## 계약 항목 판정

| 항목 | 판정 | 근거 |
|---|---|---|
| (a) DONTWAIT 1회, OK/ID 0 즉시 완료 | 통과 | `completion_owner.ts:218-257`에서 한 번 제출하고 `Ok`와 ID 0을 확인한 뒤 공유된 완료 Promise를 반환한다. completion entry나 poll 대기를 만들지 않는다. |
| (b) BACKPRESSURED payload 보유와 정확한 WRITABLE 재전송 | 통과 | `completion_owner.ts:267-295`에서 EAGAIN과 nonzero token일 때만 payload와 RID snapshot을 보유한다. `:483-517`에서 completion ID, user context, RID를 모두 비교한다. 재전송이 다시 막히면 `:559-571`에서 이전 ID를 제거하고 새 ID로 계속 기다린다. |
| (c) WRITABLE TERMINAL 전달 | 통과(수정) | `completion_owner.ts:505-515`가 TERMINAL을 `SubmitError`로 전달한다. `error_mapping.ts:46-49`는 ENOENT→NotFound, ESHUTDOWN/ECANCELED/ETERM→Terminated로 변환한다. |
| (d) ROUTER/STREAM no-route 즉시 실패 | 통과 | `completion_owner.ts:260-266`은 Backpressured가 아닌 native 결과를 즉시 거절한다. 공개 API 회귀 테스트 `routed_async_admission.test.ts:49-65`가 ROUTER no-route의 NotConnected와 Message ownership 보존을 확인한다. |
| (e) 단일 completion drain과 혼합 queue correlation | 통과 | `completion_owner.ts:411-420`이 NO_DATA까지 drain한다. `:470-480`은 nonzero context를 우선하여 REQUEST/WRITABLE을 해당 entry로 보낸다. `poller.ts:120-175`는 native registration token으로 socket을 O(1) 조회하고 POLLOUT/POLLCOMPLETION level event를 처리한다. |
| (f) close/term 정리와 thread safety | 통과(수정) | `completion_owner.ts:427-450`이 모든 waiter를 typed failure로 종료하고 token/snapshot/map/timer/poller/event를 정리한다. Node isolate의 단일 event-loop 구간에서 map을 갱신하며 await나 callback 재진입 지점이 없다. context shutdown terminal도 공개 API 테스트로 통과했다. |
| (g) 오류 mapping과 예외 타입 | 통과(수정) | ESHUTDOWN 누락을 추가했다. ENOENT/ESHUTDOWN/ETERM regression은 `send_completion_operation_path.test.ts:44-64`, 실제 shutdown은 `dontwait_backpressure.test.ts`에서 확인했다. |
| (h) REQUEST/blocking/PUB 회귀 | 통과 | 전체 test와 sample이 통과했다. REQUEST async/sync, blocking `submit_sync`, PUBSUB single/multi smoke가 모두 정상이다. |

## 발견 버그

| 파일:행 | 증상 | 수정 |
|---|---|---|
| `src/zlink/runtime/messaging/completion_owner.ts:218-295` | 성공하는 SEND마다 `CompletionEntry`, 새 Promise, `sendRetries`/`byToken` 등록·삭제와 RID 복사가 실행되어 DEALER_ROUTER가 28,373 msg/s까지 하락했다. | 첫 native DONTWAIT 결과가 `Ok`이면 공유 완료 Promise를 즉시 반환한다. entry, map, RID/payload snapshot은 실제 Backpressured 결과에서만 만든다. |
| `src/zlink/runtime/messaging/completion_owner.ts:596-654` | live waiter가 있지만 event가 없을 때 zero-timeout native poll과 `setImmediate`를 계속 반복하여 event loop와 CPU를 점유했다. 마지막 waiter가 끝나도 internal poller가 socket close까지 남았다. | 유휴 poll 횟수에 따라 0/1/2/4/8ms로 제한된 adaptive backoff를 사용하고, event 처리 시 즉시 0으로 되돌린다. 마지막 waiter가 끝나면 internal poller/event를 닫는다. |
| `src/zlink/runtime/messaging/completion_owner.ts:423-425` | public poller hot path가 POLLOUT마다 `sendRetries`를 선형 순회했다. | live WRITABLE waiter 수를 counter로 관리하여 O(1)로 바꿨다. |
| `src/zlink/runtime/messaging/completion_owner.ts:427-449` | 예약된 internal pump callback이 socket close 뒤까지 남을 수 있었다. | timer/immediate handle을 보유하고 close에서 종류에 맞게 취소한다. |
| `src/zlink/runtime/errors/error_mapping.ts:46-49` | WRITABLE TERMINAL의 ESHUTDOWN(108)이 `InternalError`로 노출됐다. | ESHUTDOWN을 `SubmitResult.Terminated`로 변환한다. ENOENT와 ETERM도 regression test에 고정했다. |

## 성능 판정

핫패스에서 필수 native submit과 payload의 native `zlink_msg` 구성 외에 새 payload snapshot, completion map 조회, poller 생성/파괴는 실행되지 않는다. JavaScript payload 복사는 실제 Backpressured 거절 뒤에만 발생한다. 대기 자료구조 lookup과 POLLOUT waiter 판정은 O(1)이다. event가 없을 때 zero-timeout spin은 하지 않는다.

DEALER_ROUTER, tcp, 1024B, duration 3초, runs 1, 동일 Core 조건:

| 측정 | throughput (msg/s) | mean latency (ms) | 판정 |
|---|---:|---:|---|
| 감독관 pre-port 기준 | 112,715 | 102.900 | 종료 하한 |
| 수정 전 current | 28,373 | 219.261 | 4배 회귀 |
| pre-port worktree A | 152,844.667 | 52.271 | 동일 명령 재측정 |
| 수정본 B | 145,420.667 | 55.264 | 감독관 하한 대비 +29.0%; 수정 전 대비 5.13배 |

수정본은 같은 시점의 A보다 throughput 4.9% 낮고 latency 3.0ms 높지만, 요구된 pre-port 하한을 넘었다. 결과 파일은 `perf/results/single/report/perf_node_single_linux_20260904_202245_c016-after.txt`이며, A 결과는 pre-port worktree의 `perf_node_single_linux_20260904_202349_c016-pre-port.txt`다.

## 검증과 smoke

| 검증 | 결과 |
|---|---|
| native addon | `ZLINK_CORE_SOURCE=local`, jobs 3으로 `scripts/rebuild_native.sh` 통과. 기존 Core만 link했다. |
| TypeScript | `npm run build` 통과. |
| 회귀 5회 | completion/backpressure/routed 관련 30개 test를 5회 연속 통과. 추가 no-route 공개 API test 11개 묶음도 5회 연속 통과. test에 고정 sleep을 추가하지 않았다. |
| 전체 test | `bash bindings/node/tests/run_tests.sh` 통과. |
| sample | 7/7 통과. |
| diff | `git diff --check -- bindings/node` 통과. TypeScript build가 tracked `dist-tools/tests` mirror를 다시 생성했고 source-layout test가 통과했다. |

Single runner smoke, 1024B, duration 2초, runs 1:

| pattern/transport | throughput (msg/s) | mean latency (ms) | 결과 |
|---|---:|---:|---|
| PAIR/tcp | 192,561.0 | 33.647 | 통과 |
| DEALER_ROUTER/tcp | 151,774.5 | 49.737 | 통과 |
| PUBSUB/tcp | 196,371.5 | 21.661 | 통과 |
| PAIR, DEALER_ROUTER, PUBSUB/inproc | - | - | runner가 `UNSUPPORTED`로 분류, 전체 status complete |

Multi runner smoke는 documented CLI option으로 CCU 8, duration 2초, sizes 1024/65536, patterns DEALER_DEALER/DEALER_ROUTER_SENDSEND/PUBSUB, timeout 300초를 적용했다. 24/24 조합이 성공했고 fail/skip/unsupported는 0이다.

| pattern/transport | throughput 1024/65536 (msg 또는 ops/s) | mean latency 1024/65536 (ms) |
|---|---:|---:|
| DEALER_DEALER/tcp | 197,299.5 / 47,876.0 | 28.945 / 30.730 |
| DEALER_DEALER/tls | 210,315.5 / 35,213.0 | 184.020 / 4.719 |
| DEALER_DEALER/ws | 193,391.5 / 46,977.0 | 115.644 / 31.411 |
| DEALER_DEALER/wss | 200,987.5 / 27,846.5 | 250.509 / 4.764 |
| DEALER_ROUTER_SENDSEND/tcp | 37,824.5 / 23,434.0 | 269.742 / 33.110 |
| DEALER_ROUTER_SENDSEND/tls | 41,535.5 / 15,022.5 | 254.934 / 32.948 |
| DEALER_ROUTER_SENDSEND/ws | 42,767.5 / 24,631.5 | 254.068 / 29.987 |
| DEALER_ROUTER_SENDSEND/wss | 44,894.5 / 12,142.5 | 245.543 / 62.524 |
| PUBSUB/tcp | 187,566.0 / 51,283.0 | 88.371 / 19.186 |
| PUBSUB/tls | 179,231.5 / 38,574.0 | 57.692 / 30.684 |
| PUBSUB/ws | 174,203.0 / 52,369.0 | 58.308 / 22.715 |
| PUBSUB/wss | 186,522.5 / 32,729.0 | 51.684 / 30.271 |

Multi 결과 파일: `perf/results/multi/report/perf_node_multi_linux_20260904_202920_c016-smoke.txt`.

## BLOCKERS

없음.
