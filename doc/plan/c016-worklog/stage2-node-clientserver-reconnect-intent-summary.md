# Node ClientServer reconnect intent 결과

ClientServer의 일반 transport loss와 admission transport 실패에서 DEALER와 Core connect intent를
유지한다. 잘못된 pushed control과 liveness 만료는 ready를 해제하고 endpoint 종료를 요청한다.
해당 endpoint의 close 관찰 뒤 intent를 한 번 복원하고, Core READY 뒤 service handshake를 다시
수행한다. 진단은 [Stage 1 기록](./stage2-node-clientserver-reconnect-intent-diagnosis.md)에 있다.
Commit과 Core/binding 재빌드는 하지 않았다.

## Diff

| 변경 파일 (`framework/languages/node/` 기준) | 변경 |
|---|---|
| `packages/framework/src/runtime/channels/channel-socket-registry.ts` | liveness의 즉시 disconnect/connect와 callback별 `reconnectOnTermination`을 제거했다. malformed control과 liveness의 종료 요청을 한 곳에서 처리하며 해당 endpoint의 close만 intent 복원을 허용한다. 일반 disconnect에는 connect를 호출하지 않는다. 이전 manual admission의 실패가 새 ready 상태를 해제하지 않도록 기존 attempt fence를 적용했다. READY(value=0) 종료 snapshot은 새 handshake를 시작하지 않는다. |
| `packages/framework/src/runtime/channels/client-server-location-runtime.ts` | 종료 시 DEALER dispose·Store 재발견을 통한 재생성을 제거했다. 기존 target의 logical admission을 pending으로 바꾸고 늦은 attempt 결과를 차단한다. transport admission 실패는 보고하고 socket을 유지한다. configuration 거부는 socket을 정리하되 같은 descriptor의 Store poll로 재시도하지 않는다. |
| `test/contract/client-server-location-runtime.test.js` | malformed control·금지 pushed command·liveness 만료의 종료 순서, 다른 endpoint close 무시, 단발 intent 복원, TCP/inproc native 재승인, 일반 disconnect의 connect 0회, 자동 discovery의 socket 보존·늦은 admission 차단을 검증한다. 기존 assertion은 유지하고 검증을 추가했다. |

`disconnectClientServerConnection`과 disposal의 `disconnectTransport` 옵션도 사용자가 없어 제거했다.
신규 public API, reconnect timer, 두 번째 poller와 native connection_id mapping은 없다.
`terminationRequested`는 endpoint 종료 요청과 close 관찰 사이의 Framework 상태이며,
callback마다 존재하던 reconnect 정책을 대체한다.

Source·contract diff는 **+333 / -58**이며, 별도로 요청된 diagnosis·summary 문서를 작성했다.

## 소유권 판정

- 소유 계층: Core — command progress·connect intent·physical pipe 교체; Framework — endpoint 종료 요청·close 관찰·handshake·descriptor 검증·logical liveness.
- Spec 조항: Framework `02-channel-transport/05-transport-liveness.ko.md` §5·§6 (`3a2ada8187`); Core socket README §4 RID 중복 정책·§6 connect/disconnect, `core/05-polling.ko.md` §3, `core/06-monitoring.ko.md` §3.1·§3.2; 구현 선행 조건 `0c39ed2e52`·`7cbf12de41`.
- 교차언어 대조: .NET `05b53a8098`의 `ZLinkClientServerClientRuntime.cs:990-1014`처럼 일반 장애의 intent를 유지하고 명시적 종료의 close 뒤에만 intent를 한 번 복원한다. 같은 파일 `:964-969`처럼 READY value 0을 제외한다. Node의 반복 재생성과 READY snapshot 오해는 언어의 구조적 차이가 아니라 책임 중복·기존 결함이었다. 다른 언어는 수정하지 않았다.
- 변경 분류: **A — 하위 계층 계약 적응**, malformed pushed control의 endpoint 종료 누락과 READY(value=0)의 admission 시작은 **B — 기존 결함**.
- 수정 전/후 규칙 수: **물리 복구 결정 4 → 2**. Core auto reconnect·Framework 즉시 재등록·discovery 종료 후 재생성·admission 실패 후 재생성에서 Core auto reconnect와 명시적 종료의 close 뒤 intent 1회 복원으로 줄였다. Framework의 handshake·liveness 규칙은 유지한다.

명시적 `disconnect(endpoint)`는 connect intent를 삭제한다(Core socket README §6).
따라서 이 결과는 모든 `Connect` 호출을 없앴다는 뜻이 아니다. 요청이 지정한 .NET commit에도
같은 단발 복원이 존재한다. 일반 장애의 재등록과 timer/실패에 따른 connect loop는 없다.

## 검증 결과

로그 디렉터리: `/tmp/zlink-stage2-node-reconnect-intent/`.

| 검증 | 결과 | 로그 |
|---|---|---|
| 최종 변경 contract 전체 | **36/36 통과**, skip·cancel 0 | `contract-final.log` |
| 최종 native TCP/inproc 회귀 5회 | **10/10 통과** | `native-final-1.log` ~ `native-final-5.log` |
| 최종 `npm run typecheck` | **통과** | `typecheck-final.log` |
| 최종 변경 TypeScript 파일·contract JS ESLint | **통과** | `lint-final.log` |
| 전체 `npm test` 1회 | **1,574/1,575 통과**, inproc 회귀 1건 실패 후 원인 수정·focused 재검증 완료 | `npm-test.log`, `npm-test.exit` |
| SupportChat.Ts 1회 | **실패(exit 1)**, support role cleanup SIGKILL | `supportchat.log`, `supportchat.exit` |
| ShoppingMall.Ts 1회 | **통과(exit 0)**, `shoppingmall-placement=completed`, `PASS ShoppingMall.Ts` | `shoppingmall.log`, `shoppingmall.exit` |
| `git diff --check` | **통과** | 최종 diff |

전체 gate는 build·typecheck·전체 lint를 통과했고 TAP announced/completed가 모두 1,575였다.
실패는 native inproc ClientServer의 handshake 기대 2회·실제 3회 한 건이다. Monitor 증거는
`DISCONNECTED → READY(value=0) → READY(value=1)`이며 Node가 value 0 snapshot도 admission으로
처리한 것이 원인이다. 기존 value를 사용하는 수정 뒤 최종 contract 36건이 통과했다.
기존 timeout과 assertion은 유지했다. 전체 `npm test`는 요청대로 한 번만 실행했으므로
**최종 수정 후 전체 gate의 성공을 주장하지 않는다**.

모든 실행은 `framework/languages/node`에서 `TMPDIR=/dev/shm/zlink-tmp-node`,
`unset ZLINK_LIBRARY_PATH`, `flock -w7200 /tmp/zlink-node-gate.lock`으로 수행한다.
Focused contract 전에는 TypeScript project build만 실행했으며 native package는 재사용했다.

- 사용 중인 Node package: `@zlink-systems/zlink` 0.17.0.
- 설치 native library SHA-256: `e8c86fc6314fc073bea8f75c027f57cfa1f8fe4f92a6113865f987f65bf10530` — 설치 package provenance와 일치.
- package archive SHA-256: `8cd720959634edb2408622c3ad73b8f55c106df87bafd2e7b598e8bf86162b7d`.
- Native 회귀 ROUTER는 .NET counterpart의 HANDOVER 설정을 사용한다. Node 운영 ROUTER의 다른 정책에 관한 새 보장을 주장하지 않는다.

작업 시작 시 존재하던 binding provenance·미추적 디렉터리와, 검증 중 추가된 다른 작업의
.NET/Node sample runner·sample regression 변경은 수정하지 않았다. 전체 gate와 sample은
공유 작업공간의 해당 병행 변경을 포함한다.

## SupportChat 종료 실패

Runner는 `samples/run-sample.mjs:490-497`에서 SIGINT 후 기존 500 ms 유예 시간을 기다리고,
남아 있는 support process를 SIGKILL했다. `:502-506`의 새 teardown 판정이 exit 1을 반환했다.
이 runner 변경은 병행 작업 소유이며 이번 작업에서 수정하지 않았다. Support role은
`samples/SupportChat.Ts/Server/Support/main.ts:16`의 `app.close()`로 종료하고 RouteMesh와
ClientServer client를 함께 사용한다. 보존된 로그만으로 어느 종료 단계가 지연됐는지,
이번 ClientServer 변경과 인과관계가 있는지 또는 Core/binding 결함인지는 확정할 수 없다.

브라우저 로그 `browser-client.log:107-109`에는 scenario 완료가 있지만 이것을 sample 성공으로
계산하지 않았다. Support의 기존 message-flow 파일에는 마지막 lifecycle relay의
received/admitted/dispatched/replied 기록이 있으며, scenario가 의도한 handler 거부 외에
종료 단계의 원인을 특정하는 오류는 없다. timeout·retry·assertion과 runner 판정을 변경하지
않았으며, 요청대로 sample을 재실행하지 않았다.

- SupportChat run dir: `/dev/shm/zlink-tmp-node/zlink-supportchat.ts-oRfE14/`.
- ShoppingMall run dir: `/dev/shm/zlink-tmp-node/zlink-shoppingmall.ts-moQmn1/`.
- 두 실행은 기존 `samples/run-sample.mjs`에 각 `Runner/sample-runner.mjs`와 `--keep-run-dir`을 전달했다. 역할별 로그와 기존 `logs/flow/`를 보존했다.
- 로그 사본: `/tmp/zlink-stage2-node-reconnect-intent/supportchat-evidence/`, `shoppingmall-evidence/`.

## BLOCKERS

확인된 Core/binding 결함은 없다. 전체 gate의 유일한 실패는 Node READY snapshot 해석 결함으로
수정했고 최종 ClientServer contract 36/36과 native 반복 10/10이 통과했다. 최종 수정 후 전체
gate 재실행은 요청된 1회 제한에 따라 생략했다.

**SupportChat.Ts의 clean teardown 성공은 차단돼 있다.** support role의 SIGKILL 원인은 위
로그로 담당 supervisor가 후속 분리해야 한다. ShoppingMall.Ts는 통과했다. Sample 성공 전체와
최종 전체 gate 성공을 주장하지 않는다.
