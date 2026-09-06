# Node binding R3 수정 결과

2026-09-06. F-R3-1·F-R3-12 수정 완료. F-R3-16은 진단을 확인했으나 실행 모델 결정이 필요해 미완료다. 전체 gate 통과가 F-R3-16 해결을 뜻하지 않는다.

- 작업 branch: `main`. commit·push 없음. 최종 검증 시 HEAD: `a4f8cde0ef`.
- 변경 범위: Node binding 구현 1개, 원인별 회귀 테스트 2개와 `dist-tools` 생성 결과, 이 보고서.
- 기존 다른 binding·Framework 변경을 보존했다. Core·spec·Framework·site·다른 binding은 수정하지 않았다.
- 루트 `AGENTS.md`, `doc/AGENTS.md`, D-098·D-109·D-111, POSDDD·시스템 설계 원칙을 적용했다. `bindings/AGENTS.md`와 Node 하위 `AGENTS.md`는 없다.

## F-R3-1 — WRITABLE의 RID 재검증 제거

- 소유 계층: Core가 submit RID echo를 보장하고, Node completion owner는 socket-local context·token으로 waiter를 찾는다.
- Spec 조항: `bindings/doc/spec/README.ko.md:1341`, Core socket README의 part send(`:989`), completion ownership(`:1149`), `async-coroutine-policy.ko.md` §3·§4.
- 원인: 수정 전 `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:529`의 `sameTarget` 계산과 `:539`의 추가 실패 조건. 이미 찾은 waiter를 RID 불일치로 다시 거부했다.
- 수정: `captureWritable`에서 retry 조회와 RID 비교를 제거했다. kind·completion ID·context 검사는 유지했다. Node의 이 비교 자체에는 별도 `RoutingId` 객체 생성이 없었다. 재제출 target을 보관하는 `Buffer.from(routingId)`는 재제출에 필요하다. Native completion의 RID 출력도 기존 raw 계약 테스트 `send_completion_boundary.test.ts:149`가 직접 검증하므로 유지했다. 기존 assertion은 바꾸지 않았다.
- 교차언어 대조: Python `routed_async.py:838`의 현재 `_capture_writable`도 RID를 읽지 않고 token·context·kind로 전달한다. C `bindings/c/include/zlink/socket/api.h:225`는 Core raw 계약을 그대로 제공한다. Node만의 target 판정은 필요하지 않다.
- 변경 분류: **B — 기존 결함**, 하위 계층 보장의 중복 검사 제거. 정상 Core의 공개 동작은 동일하다.
- 수정 전/후 규칙 수: **2 → 1** — Core echo 보장 + binding 재검증 → Core echo 보장 하나. Node 범위의 판정 소유자를 센 값이다.

회귀 테스트: `bindings/node/tests/writable_token_delivery.test.ts` 4건. SEND·REQUEST 각각 routed/unrouted target을 사용하고, native 경계 fixture의 `peerRoutingId` getter가 호출되면 실패한다. 해당 token의 WRITABLE을 전달하면 원래 target으로 정확히 한 번 재제출하고 waiter가 성공해야 한다. 수정 전 4건 모두 RID 접근에서 실패, 수정 후 4/4 통과. F-R3-12 없이도 독립적으로 통과함을 확인했다.

Diff 분리: `completion_owner.ts`의 `captureWritable` 시작부 `sameTarget` 제거 hunk + 위 테스트의 `.ts`와 `bindings/node/dist-tools/tests/writable_token_delivery.test.js`. 다른 hunk 없이 F-R3-1로 분리할 수 있다.

Gate 결과: 아래 전체 gate 141/141, 샘플 7/7에 포함. 새 회귀 테스트 묶음 5회 중 매회 이 원인의 4/4 통과.

BLOCKERS: 없음.

## F-R3-12 — NO_DATA 뒤에만 재제출

- 소유 계층: socket의 지정된 Node completion owner가 drain과 언어 waiter 재제출 순서를 소유한다. wake 조건과 admission 결과는 Core가 소유한다.
- Spec 조항: Core socket README part send `:992`, REQUEST DONTWAIT `:1075`, completion ownership `:1177`; `bindings/doc/spec/async-execution-model.ko.md` §4; `async-coroutine-policy.ko.md` §4.
- 원인: 수정 전 `completion_owner.ts:453`의 recv loop가 `captureWritable:559`에서 `attemptRetry`를 즉시 호출했다. 재제출이 만든 completion까지 같은 drain에서 소비했다.
- 수정: WRITABLE capture는 `writableRetries`에 해당 entry만 모은다. `drain()`이 DONTWAIT recv에서 `NO_DATA`를 받은 뒤 이 목록을 처리한다. 새 completion은 다음 drain에 남는다. 재제출 목록은 처리 후와 lifecycle/error 정리에서 비운다. retained payload의 기존 `retries` map은 그대로 사용하며, 목록에 payload·target·새 retry 정책을 복제하지 않는다.
- 기존 sync bridge가 반환한 completion 배열도 `captureWritable`을 거친다. 따라서 `requestSync`의 capture 뒤 기존 owner의 `drain()`을 호출해 같은 NO_DATA 경계를 사용한다. `drain`의 public owner 검사는 그대로 적용한다. 이 한 줄은 **F-R3-12** 수정이며 native sync 소비자 제거인 F-R3-16을 해결하지 않는다.
- 대안 비교: drain 중 즉시 재제출을 유지하는 방법은 계약 위반이다. 기존 pending map 전체를 매번 스캔하거나 entry에 별도 ready flag를 넣는 대신, .NET처럼 이미 받은 WRITABLE의 entry만 모아 drain 경계에서 처리했다. 새 poller·timer·timeout·재시도 횟수 정책은 없다.
- 교차언어 대조: .NET `CompletionOwner.cs:331`의 `DrainCore`가 `NoData`를 받은 뒤 `_retries`를 처리하는 D-B117 구현과 동일한 순서다. Python도 `_capture_writable`과 drain 이후 재제출을 분리한다. Node의 기존 inline 호출은 구조상 필요한 차이가 아니었다.
- 변경 분류: **B — 기존 결함**, Core의 기존 drain 순서 복원.
- 수정 전/후 규칙 수: **2 → 1** — capture 즉시 재제출 / NO_DATA 뒤 재제출 → owner의 NO_DATA 뒤 재제출 하나.

회귀 테스트: `bindings/node/tests/completion_drain_order.test.ts` 3건.

1. SEND WRITABLE 뒤 다른 REQUEST completion을 queue에 넣는다. `WRITABLE → 다른 completion → NO_DATA → 재제출` 순서를 확인한다. 재제출이 만든 두 번째 WRITABLE은 다음 drain에 남아야 한다.
2. REQUEST에도 같은 순서를 확인한다. 최종 재제출이 만든 REQUEST 결과 역시 다음 drain에 남아야 한다.
3. 기존 native sync bridge에서 건네받은 다른 SEND의 WRITABLE도 owner가 나머지 completion을 읽어 NO_DATA를 확인한 뒤에만 재제출해야 한다.

수정 전 3건 모두 실패했다. SEND는 한 drain에서 2건 대신 3건, REQUEST는 2건 대신 4건을 소비했다. 수정 후 3/3 통과했다.

Diff 분리: `completion_owner.ts`의 `writableRetries` 필드, `requestSync` capture 뒤 `drain()`, `drain`의 NO_DATA 이후 목록 처리, `captureWritable` 마지막 줄의 enqueue, `close`·`runtimeWake` 정리 hunk + 위 테스트 `.ts`와 `bindings/node/dist-tools/tests/completion_drain_order.test.js`. F-R3-1의 `sameTarget` 제거와 별개 원인이다.

Gate 결과: 아래 전체 gate와 샘플에 포함. 새 회귀 테스트 묶음 5회 중 매회 이 원인의 3/3 통과.

BLOCKERS: F-R3-12 자체에는 없음. F-R3-16의 native sync 소비자가 public owner를 우회하는 기존 경로는 남아 있다.

## F-R3-16 — 지정된 owner를 통한 동기 대기: BLOCKED

- 소유 계층: `async-execution-model`이 지정한 runtime owner 또는 public poller의 `wait()` thread.
- Spec 조항: `bindings/doc/spec/async-execution-model.ko.md:67`–`:82` (§4), `async-coroutine-policy.ko.md` §1·§4, Core socket README completion ownership `:1177`–`:1189` 및 §6 completion 표의 `REQUEST_TIMED_OUT`.
- 원인: 수정 전 `completion_owner.ts:375`–`:405` / 현재 `:376`–`:407`은 public owner와 관계없이 `socketRequestSync`를 호출한다. `bindings/node/native/src/addon_core.cc:2477`–`:2537`은 자체 `zlink_completion_recv(NONE)` loop로 자기 ID까지 읽는다. NO_DATA를 `throw_last_error`로 바꾼 뒤 TypeScript가 이를 submit error로 변환한다.
- 교차언어 대조: .NET `CompletionOwner.cs:224`의 `Request`는 entry Task를 기다리고 별도 진행 가능한 owner가 완료한다. Python `routed_async.py:1257`의 대기는 public owner가 있으면 condition에서 기다린다. 두 언어는 같은 socket·owner 객체를 다른 실행 thread와 공유할 수 있다. Node는 이 작업에서 사용한 공개 객체를 Worker로 공유할 방법이 없다.
- 변경 분류: **B — 기존 결함으로 진단 확인, F-R3-16 구현 없음**. Node의 공개 thread/ownership 투영에 필요한 설계 결정을 임의로 추가하지 않았다.
- 수정 전/후 규칙 수: **2 → 2 (미해결)** — 지정 owner와 native sync 소비자가 모두 남는다. 목표는 2 → 1이다.

### 실행 모델의 제약

`runtime/eventing/poller.ts:159`의 `wait()`는 동기 native wait가 반환한 뒤 같은 JS thread에서 `:200`의 `owner.drain(this)`를 실행한다. `completion_owner.ts:195`의 entry registry와 Promise도 이 thread에 있다. `runtime/handles/native_handle.ts:3`은 JS WeakMap을 사용한다. `runtime/eventing/thread.ts:17`은 handler의 소스를 새 Worker에서 평가하며 caller의 closure·poller 객체를 공유하지 않는다.

따라서 `socketRequestSync`를 submit-only로 바꾸고 JS Promise/entry를 blocking wait하는 변경만으로는 public `wait()`를 진행할 수 없다. 실제 public poller 객체를 `MessagePort.postMessage`로 보내면 `DataCloneError`가 발생했다. Node-API도 Worker 사이의 `napi_env` 공유를 허용하지 않는다. [Node 22.23.2 Node-API 문서](https://nodejs.org/download/release/v22.23.2/docs/api/n-api.html#napi_env).

검토한 두 방향은 다음과 같다.

- 기존 owner를 유지한 채 entry만 blocking wait: Node에서 그 owner를 다른 thread가 구동할 공개 API가 필요하다. 현재 공개 interface만으로 요청된 병행 테스트를 구성할 수 없다.
- 동기 함수가 public poller를 대신 호출하거나 event loop를 재진입: public `wait()` 실행 조건과 다른 event 전달 규칙을 바꾸게 된다. 특히 `uv_run()`은 callback 안의 재진입을 금지한다. 별도 poller·drain thread 또는 busy wait로 보상하지 않았다. [libuv loop 문서](https://docs.libuv.org/en/v1.x/loop.html#c.uv_run).

이 판단은 소유권 요구를 완화할 근거가 아니다. 루트 `AGENTS.md` §3의 “새 public API가 필요해 보이면 … 구현 전에 설계 변경으로 분리해 사용자에게 보고한다”에 따라 감독자에게 실행 모델 결정을 요청했다. 이 보고서 작성 시 답변은 없었으며, 그 부재를 승인으로 간주하지 않았다.

### 공개 API 진단과 남은 실패

진단 스크립트: `/tmp/zlink-node-r3-request-sync-repro.cjs`.
출력: `/tmp/zlink-node-r3-request-sync-repro.log`.

연결·초기 SEND/recv를 완료한 DEALER–ROUTER에서 responder가 reply하지 않도록 하고 `dealer.request().message('no-reply').timeout(50).submit_sync()`를 실행했다.

| DEALER `recvTimeout` | 기대 | 실제 |
|---|---|---|
| 0 ms | `RequestError.TimedOut` (101) | `SubmitError.Backpressured` (1), errno 11 |
| 5 ms | `RequestError.TimedOut` (101) | `SubmitError.Backpressured` (1), errno 11 |
| 1000 ms | `RequestError.TimedOut` (101) | 기대와 일치 |

위의 처음 두 경우는 유효한 공개 API 호출에서 남는 오류 분류 결함이다. 같은 thread에서 sync request와 public wait를 직렬 호출한 추가 probe는 §4의 병행 실행 전제를 만족하지 않으므로, 그것을 올바른 owner 동작의 회귀 테스트나 실패 증거로 사용하지 않았다. `postMessage(poller)`의 `DataCloneError`는 별도로 확인했다.

회귀 테스트: F-R3-16의 완료를 입증할 새 통과 테스트는 없음. 기존 `routed_async_admission.test.ts`의 sync reply 반환 테스트는 gate에서 통과하지만 public poller 병행 소유권을 검증하지 않는다. 위 진단은 정상 gate와 분리했으며 오류 분류 불일치 때문에 exit 1이다. assertion을 완화하거나 skip된 테스트를 추가하지 않았다.

Diff 분리: F-R3-16용 구현·native diff 없음. 보고서의 이 절과 `/tmp` 진단 자료만 해당한다. F-R3-12의 `requestSync` 한 줄을 F-R3-16 완료로 분류하면 안 된다.

BLOCKERS: 같은 socket의 지정 public poller를 다른 Worker에서 실행할 수 있는 수명·공유 계약, 또는 Node의 blocking terminal/public poller 조합에 대한 별도 계약 결정을 먼저 확정해야 한다. 현재 요구된 공개 API 유지·별도 poller/thread 금지·owner 진행 보장을 모두 만족하는 구현은 확인하지 못했다. Core timeout이나 retry 횟수를 늘리는 변경으로 해결할 문제가 아니다.

## 검증

Node `v22.23.2`, local Core `core/build-dev/lib/libzlink.so.0.17.0`을 사용했다. Core 파일은 변경하지 않았다.

- Core SHA-256: `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`.
- Node addon은 현재 source로 다시 빌드했다. SHA-256: `890596d6efdc2b4ec7bbf86ae642665f6415276f4c24aa5c0a26bb5406f330f9`.
- `build/Release/libzlink.so.0`의 실제 경로와 `ldd`로 지정 Core 연결을 확인했다.
- local helper의 기본 경로는 존재하지 않는 `core/build/lib`이므로, addon 빌드에 `ZLINK_CORE_SOURCE=local`, 절대 `ZLINK_CORE_INCLUDE_DIR`, `ZLINK_CORE_LIB_DIR=.../core/build-dev/lib`를 넘겨 `node-gyp configure build`를 실행했다. 공유 helper나 Core 경로는 수정하지 않았다.

| 검증 | 결과 | 로그 |
|---|---|---|
| native addon rebuild | exit 0 | `/tmp/zlink-node-r3-native-build.log` |
| `npm run build:incremental` | exit 0 | `/tmp/zlink-node-r3-build.log` |
| `npm run typecheck` | exit 0 | `/tmp/zlink-node-r3-typecheck.log` |
| 관련 7개 test file | 41/41, 실패 0 | `/tmp/zlink-node-r3-related.log` |
| 새 테스트 7개 × 5회 | 35/35, 매회 실패 0 | `/tmp/zlink-node-r3-regressions-{1,2,3,4,5}.log` |
| `tests/run_tests.sh` | 31개 file, 141/141, 실패·skip 0 | `/tmp/zlink-node-r3-gate.log` |
| 같은 script의 binding samples | 7/7, 실패 0 | 위 gate log |
| `git diff --check -- bindings/node` | 통과 | 명령 결과 |
| F-R3-16 공개 API 진단 | 오류 분류 2건 미해결, exit 1 | `/tmp/zlink-node-r3-request-sync-repro.log` |

Gate는 `bindings/node`에서 다음 환경으로 **한 번** 실행했다. 전체 실행 동안 sample lock을 유지했다.

```bash
ZLINK_CORE_SOURCE=local \
ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0.17.0 \
LD_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib \
flock /tmp/zlink-samples-gate.lock bash tests/run_tests.sh \
  > /tmp/zlink-node-r3-gate.log 2>&1
```

성능 benchmark runner는 실행하지 않았다. 기본 gate에 포함된 `perf_single_worker_contract.test.js`의 기능 검증은 그대로 실행했다.
