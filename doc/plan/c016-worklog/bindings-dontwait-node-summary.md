# Node DONTWAIT backpressure 계약 정합 결과

## 결과

Node 바인딩의 managed SEND를 D-B79 확정 B 계약에 맞췄다. `send(...).submit()`은 먼저 DONTWAIT로 제출한다. 정상 admission은 completion ID 0에서 끝나며 SEND completion을 기다리지 않는다. HWM 때문에 Core가 `SubmitResult.Backpressured`, `EAGAIN`, nonzero wait token을 반환하면 바인딩이 submit 시점의 packet snapshot과 target을 보존한다. `Message` wrapper는 snapshot 직후 비워 caller가 다시 사용할 수 있다.

public poller가 completion queue를 소유하지 않을 때는 Node event-loop turn마다 nonblocking poller probe를 수행한다. `PollEventFlag.PollOut` 뒤 queue를 더 읽을 record가 없을 때까지 비우고, token·user context·RID가 모두 같은 `CompletionKind.Writable`을 확인한 뒤 같은 packet을 다시 제출한다. 재제출이 정상 admission되면 ID 0에서 Promise를 완료한다. 별도 OS thread, sleep, timer는 사용하지 않는다.

REQUEST/reply는 기존 completion queue 흐름을 유지한다. REQUEST correlation은 completion ID와 user context를 함께 검사하며 Core `ETERM`을 `RequestResult.Terminated`로 변환한다.

## API 전후 비교

| 영역 | 이전 동작 | 변경 후 동작 |
|---|---|---|
| `send(...).submit()` | ID 0에는 synthetic SEND completion을 만들고, nonzero ID는 SEND completion을 기다렸다. | DONTWAIT 정상 admission은 ID 0에서 즉시 완료한다. BACKPRESSURED/EAGAIN이면 binding-owned snapshot을 보존하고 WRITABLE 뒤 같은 packet을 다시 제출한다. |
| BACKPRESSURED payload | Core가 accepted pending SEND payload를 소유한다는 전제를 사용했다. | Core는 payload를 보존하지 않는다. 바인딩이 최초 backpressure에서 immutable snapshot을 만든다. |
| `Message` ownership | Core가 pending SEND를 받아들인 뒤 wrapper를 비웠다. | 정상 admission 또는 최초 backpressure snapshot 직후 wrapper를 비운다. terminal failure가 그보다 먼저 발생하면 caller ownership을 유지한다. |
| `submit_sync()` | blocking admission 뒤 payload를 비웠다. | public signature와 blocking 동작은 유지하고, 정상 결과의 completion ID가 0인지 검증한다. |
| completion record | Node object에 WRITABLE target RID가 없었다. | native completion object가 `peerRoutingId`를 복사해 제공한다. token·context·RID를 모두 비교한다. |
| `CompletionKind` | public root에 completion kind enum이 없었다. | `Send: 1`, `Request: 2`, `Writable: 3`을 value/type으로 노출한다. `Send`는 ABI-only 값이며 runtime record는 REQUEST와 WRITABLE을 사용한다. |
| `Poller.wait()` | `PollCompletion` 중심으로 queue를 drain했고 user slot을 native token으로 사용했다. | WRITABLE의 `PollOut`에서도 managed SEND queue를 drain한다. 내부 registration token과 user slot을 분리하고, completion owner를 단일 public poller에만 이전한다. |
| `ZLINK_OPT_PENDING_MAX_*` | SEND와 REQUEST 범위가 문서에서 구분되지 않았다. | ABI 값은 유지한다. raw Core에서 admission을 기다리는 REQUEST record의 count/bytes 상한이며 typed Node socket option에는 노출하지 않는다. SEND retry state에는 적용하지 않는다. |
| REQUEST/reply | REQUEST completion으로 Promise를 완료했다. | 흐름을 유지한다. ID/context correlation과 `ETERM` 변환만 강화했다. |

## 변경 파일

Runtime와 public contract:

- `bindings/node/native/src/addon_core.cc`
- `bindings/node/src/zlink/runtime/messaging/completion_owner.ts`
- `bindings/node/src/zlink/runtime/eventing/poller.ts`
- `bindings/node/src/zlink/runtime/errors/error_mapping.ts`
- `bindings/node/src/zlink/runtime/options/option_mapping.ts`
- `bindings/node/src/zlink/runtime/sockets/socket_operation_builders.ts`
- `bindings/node/src/zlink/contracts/eventing/poller.ts`
- `bindings/node/src/zlink/contracts/messaging/message.ts`
- `bindings/node/src/zlink/contracts/messaging/operations.ts`
- `bindings/node/src/zlink/contracts/messaging/received.ts`
- `bindings/node/src/zlink/contracts/sockets/index.ts`
- `bindings/node/src/zlink/contracts/sockets/socket_constants.ts`

Test와 type surface:

- `bindings/node/tests/dontwait_backpressure.test.ts`
- `bindings/node/tests/send_completion_boundary.test.ts`
- `bindings/node/tests/send_completion_operation_path.test.ts`
- `bindings/node/tests/routed_async_admission.test.ts`
- `bindings/node/tests/perf_multi_routed_sendsend_contract.test.ts`
- `bindings/node/tests/hwm_contract.test.ts`
- `bindings/node/tests/public_exports.test.ts`
- `bindings/node/tests/source_layout.test.ts`
- `bindings/node/tests/socket_surface.typecheck.ts`

문서와 perf 설명:

- `bindings/node/README.md`
- `bindings/node/README.typedoc.md`
- `bindings/node/perf/multi/perf_multi_dealer_dealer_client.ts`
- `bindings/node/perf/multi/perf_multi_routed_sendsend.ts`
- `bindings/node/perf/multi/perf_multi_runtime.ts`

`npm run build`가 위 TypeScript test/perf 변경에 대응하는 `bindings/node/dist-tools/**` JavaScript를 갱신했다.

## Test와 gate

모든 build와 test 명령에는 `ulimit -v 16777216`을 적용했다. Node addon과 test process는 `ZLINK_CORE_SOURCE=local`, `ZLINK_LIBRARY_PATH=core/build-dev/lib/libzlink.so.0.16.0`, `LD_LIBRARY_PATH=core/build-dev/lib`으로 같은 Core dev library를 사용했다.

| 검증 | 결과 |
|---|---|
| generated target clean 후 `bash scripts/build-core.sh dev` | 통과, `core/build-dev/lib` 생성 |
| `node-gyp configure build` (`ZLINK_CORE_INCLUDE_DIR=core/include`, `ZLINK_CORE_LIB_DIR=core/build-dev/lib`) | 통과 |
| `npm run build` | 통과 |
| `npm run typecheck` | 통과 |
| 핵심 completion/backpressure/poller test | 27/27 통과 |
| `dontwait_backpressure` + raw `send_completion_boundary` 반복 | 5회 연속, 총 40/40 통과 |
| `bash tests/run_tests.sh` | Node test 110/110, sample 7/7 통과 |
| `git diff --check` | 통과 |
| `bindings/node/include` raw header mirror | 디렉터리가 없어 cmp 대상 없음 |
| README 2축 독립 리뷰 | 문서 원칙과 코드 계약의 high/medium finding 반영 완료 |

신규 raw test는 HWM까지 채운 뒤 BACKPRESSURED/EAGAIN과 nonzero token을 확인한다. peer drain 뒤 `POLLOUT`, 같은 token·context·RID의 WRITABLE, 같은 packet 재제출 성공, ID 0과 duplicate 부재까지 public socket/poller API와 addon 경계에서 검증한다. 신규 managed test는 Buffer mutation 격리, `Message` wrapper 재사용, routed target 보존, context shutdown terminal과 public `CompletionKind.Writable`을 검증한다. 신규 DONTWAIT/WRITABLE 시나리오에는 sleep이나 timer가 없다.

비표준 보조 검사인 `npm run typecheck:src-review`는 기존 `src/zlink/contracts/messaging/topic_message.ts`의 미사용 `_reusableSinglePartSlots`에 대한 TS6133으로 실패한다. 이번 변경과 표준 `npm run typecheck` 대상에는 포함되지 않으며 수정하지 않았다.

## 범위 경계

Core는 poller wake handle을 public API로 제공하지 않는다. 따라서 public poller가 completion ownership을 갖지 않은 managed Promise는 pending 동안 `setImmediate`와 timeout 0 poller probe로 진행한다. 이는 Node event loop에서 실행되고 별도 thread·sleep·timer를 사용하지 않지만, pending 시간이 길면 반복 probe 비용이 발생할 수 있다. signal-driven native watcher로 바꾸려면 Core wake source를 공개하는 별도 계약이 필요하다.

현재 Core snapshot은 이미 끊긴 stale route에 wait token을 반환한 뒤 terminal completion을 내지 않을 수 있다. Node test는 Core가 route를 이미 정리한 경우의 즉시 NotFound/NotConnected와, wait token을 반환한 경우 sender close에서 typed Terminated로 끝나는 ownership 경계를 모두 검증한다. 감독관이 최종 Core를 교체해 terminal completion을 제공해도 같은 managed terminal 처리 경로를 사용한다.

commit, push, checkout, `--core-version`, `scripts/local-package/**`는 실행하지 않았다.
