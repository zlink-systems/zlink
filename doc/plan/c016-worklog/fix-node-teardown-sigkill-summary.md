# Node sample teardown 수정 결과

2026-09-05. 감독의 승인된 [진단](diag-node-teardown-sigkill.md)에 따른 구현·검증 기록이다.

**승인된 Nest 종료 연결, STREAM heartbeat, runner 종료 판정을 수정했다. 완료 조건은 미충족이다.**
개별 sample은 4개 통과·3개 실패다. 기존 `shutdown()` 내부의 추가 결함 때문에 TicTacToe,
SupportChat, ZoneWorld에 cleanup SIGKILL이 남는다. `npm test`는 1,585개를 모두 실행했고
1,583개 통과·2개 실패다. 실패는 새 host 회귀 테스트의 callback 오류 분류와 deadline 정리다.

`main`에서 작업했으며 commit하지 않았다. Core·binding·다른 언어·shared_sample·보호 문서는
수정하지 않았다. 설치 package를 재설치하지 않았고 runner의 **500ms grace도 유지**했다.
기존 사용자 변경은 수정하지 않았다.

## 소유 계층별 diff

아래 `N/`은 `framework/languages/node/`를 뜻한다.

| 소유자 | 변경 파일 | 변경 내용 |
|---|---|---|
| Framework host / Nest | `N/packages/nestjs/src/providers.ts:344`, `N/packages/framework/src/runtime/host/index.ts:1722` | `onModuleDestroy()`와 `onApplicationShutdown()` 모두 기존 host `shutdown()`에 합류한다. Nest의 transport-only `stop()` 호출을 제거했다. 하위 transport 정리는 기존 `runShutdown()`이 계속 소유한다. |
| Nest composition bridge | `N/packages/nestjs/src/framework-integration-contracts.ts`, `N/packages/framework/src/nest-integration.ts` | 내부 bridge의 `stop()` 선언을 제거하고 공개 `ZLinkFrameworkRuntime`의 `shutdown` 타입을 `Pick`으로 재사용한다. 새 공개 API는 없다. |
| Framework STREAM control send | `N/packages/framework/src/runtime/streams/managed-stream.ts:157`, `N/packages/framework/src/runtime/streams/stream-session-runtime.ts` | `writeControl()`은 기존 socket `submit()`의 terminal을 기다린다. Ping·pong에 같은 경로를 사용하며 message는 terminal 뒤 닫는다. 빠른 heartbeat 처리도 기존 `handleControl()`을 재사용한다. Application handler와 독립적인 control 처리와 permit 반환을 유지하고, 비동기 오류는 기존 `onError`에 전달한다. |
| Sample runner | `N/samples/run-sample.mjs`, `N/samples/GameQuest.Ts/Runner/sample-runner.mjs:80` | GameQuest가 `await ctx.stop(owner, 'SIGKILL')`로 종료를 회수하고 실제 signal을 검증한다. 사용처가 없어진 fire-and-forget `signal()`을 제거했다. Cleanup 시작 당시 실행 중인 역할만 cleanup 실패 후보로 잡는다. Cleanup 중 이미 SIGKILL된 역할과 grace 뒤 강제 종료한 역할의 보고를 유지한다. Scenario 종료 시 기존 unexpected-exit 검사도 수행한다. |

소유 계층: host terminal·Spot scope 정리는 Framework, native admission 대기·completion·재제출은 binding, 의도한 fault injection과 cleanup 실패 판정은 sample runner다.

Spec 조항: [Node system structure §5](../../../framework/doc/framework/common/spec/server/languages/node/01-system-structure.ko.md), [host relocation flow §14](../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md), [Node binding의 Pull completion 공개 계약](../../../bindings/doc/spec/node/README.ko.md)이다. Runner 변경은 Java의 scenario 종료 회수 동작과 대조했다.

교차언어 대조: Java `ZLinkFrameworkRuntime.java:1971` / `ZLinkSpotRuntime.java:1739`는 Spot·Instance cleanup과 timer executor 종료를 수행한다. `ZLinkStreamRuntime.java:1492`는 `sendControlAsync → sendAsync`를 사용한다. Java GameQuest `run_sample.sh:326-328`은 SIGKILL → wait → cleanup 목록 제외 순서다. Node 수정 사유는 JS timer 자체가 아니라 Nest 종료 연결 누락, 동기 terminal 선택, runner 회수 누락이다. 다른 언어는 실행하지 않았다.

변경 분류: **B 기존 결함**, 승인된 진단의 구현이다. 아래 BLOCKERS는 이번 검증에서 추가로 발견한 Framework 결함이며 구현 승인을 받지 않아 수정하지 않았다.

수정 전/후 규칙 수: **6 → 3**. 비교 단위는 소유자별 종료·전송·실패 판정 경로다. Host의 `Shutdown / Nest stop`은 2 → 1, STREAM의 `async application·closing / sync heartbeat`는 2 → 1, runner의 `이전 SIGKILL / cleanup 강제 종료`는 cleanup 시작 시 실행 중인 역할의 강제 종료라는 2 → 1로 합쳤다. 새 지속 상태·timer·retry·poller는 0개다.

대안 비교: transport stop에 별도 Spot cleanup을 추가하면 종료 정책이 중복된다. Control send에 Framework 재제출을 추가하면 binding 정책이 중복된다. Runner에서 `expectedStop`별 SIGKILL 예외를 계속 추가하면 종료 분류 규칙이 늘어난다. 각각 기존 Shutdown, binding async terminal, cleanup 시작 시 실행 중인 역할이라는 기준을 사용했다.

## 회귀 테스트

| 변경·추가 파일 | 검증 내용 | 결과 |
|---|---|---|
| `N/test/contract/nestjs-shutdown.test.js`, `fixtures/nestjs-shutdown-process.js` | 실제 Nest 자식 process에서 Entry/User/Instance timer 활성화 → `app.close()` → framework 소유 active handle·참조된 timer 0개 → `process.exit()` 없이 자연 종료. Normal/shared operation의 HostShutdown callback도 확인한다. | normal/shared 통과, callback failure/deadline 실패 |
| `N/test/contract/stream-runtime.test.js` | 동기 write의 기존 계약과 분리해 heartbeat async terminal 대기·message lifetime을 검사한다. | 통과 |
| `N/test/contract/stream-heartbeat-shutdown.test.js`, `fixtures/stream-heartbeat-shutdown-process.js` | 실제 설치 binding과 데이터를 읽지 않는 TCP peer로 admission을 포화시킨다. Heartbeat Promise가 pending인 상태에서 SIGINT → socket/context 정리 → pending terminal 완료 → 자연 종료를 확인한다. | 통과, SIGKILL 없음 |
| `N/test/contract/sample-runner-teardown.test.js` | 실제 자식 process로 의도한 SIGKILL 회수, SIGINT 무응답, cleanup 중 SIGKILL, 예상하지 않은 이른 종료를 검사한다. Shared runner의 실제 함수를 VM에서 실행해 build/Redis/browser 진입만 분리한다. | 4/4 통과 |

Host fixture는 공개 Nest 등록과 공개 User Spot create를 사용한다. Entry의 지연 초기화와 Instance
materialization은 fixture에서 기존 runtime manager를 호출해 준비한다. 이 테스트의 범위는
placement가 아니라 등록된 scope의 teardown이다. 실제 sample이 공개 호출을 통한 활성화를 검증한다.
Node의 `_getActiveHandles()`는 timer를 모두 표시하지 않으므로 `async_hooks`의 생성 stack으로
Framework/binding 소유 자원을 추적하고 참조된 Timeout도 함께 검사한다.

초기 공개 Instance request로 fixture를 준비하는 시도에서는 result 105 / errno 17을 관찰했다.
이 경로의 원인은 확정하지 않았고 구현을 바꾸지 않았다. Teardown fixture에서는 기존 local
materialization으로 경계를 분리했다. Deadline fixture의 watchdog SIGKILL은 실패로 유지했다.

## 실행 환경과 검증 결과

Node v22.23.2, cwd `framework/languages/node`, `TMPDIR=/dev/shm/zlink-tmp-node`,
`ZLINK_LIBRARY_PATH` unset, `flock -w7200 /tmp/zlink-node-gate.lock`으로 직렬 실행했다.
설치 Core의 SHA-256은 작업 전·후 모두 다음과 같다.

```text
node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0.17.0
98f3499696009ee5d43a1680ab5423c306d28af7592c1ca48fb40f3ee20773eb
```

첫 TicTacToe 준비 실행은 Playwright가 요구하는 Chromium 1243 부재로 browser 시작 전에 실패했다.
`npm run browser:install`로 browser 실행 파일만 설치한 뒤 다음 개별 검증을 수행했다. npm/Core
package 재설치는 없었다.

| 개별 sample | Exit | Cleanup SIGKILL 역할 | 결과 |
|---|---:|---|---|
| TicTacToe.Ts | 1 | play-a, play-b, api-a, api-b | scenario 실행 뒤 cleanup 실패 |
| Bingo.Ts | 0 | 없음 | PASS |
| DeliveryDispatch.Ts | 0 | 없음 | PASS |
| SupportChat.Ts | 1 | support | scenario 실행 뒤 cleanup 실패 |
| GameQuest.Ts | 0 | 없음 | PASS. 의도한 owner SIGKILL은 scenario에서 회수 |
| ShoppingMall.Ts | 0 | 없음 | PASS |
| ZoneWorld | 1 | B8의 zone-node-2, gateway | B8 child cleanup 실패. full lane 미실행 |

| 검증 | 결과 |
|---|---|
| `npm run build` | 통과 |
| 기존 touched subsystem: `nestjs-module`, `stream-runtime`, `stream-session-runtime` | 272/272 통과 |
| 새 host contract | 2/4 통과. BLOCKERS의 callback failure/deadline 실패 |
| 실제 binding heartbeat / SIGINT contract | 1/1 통과 |
| Runner teardown contract | 4/4 통과 |
| `npm run typecheck` | exit 0 |
| 변경 TS·JS·MJS 파일의 ESLint | exit 0 |
| `npm test` — 한 번 | exit 1. 1,585개 실행, 1,583 pass / 2 fail / 0 cancelled / 0 skipped. TAP announced=completed=1585 |
| `bash samples/run_samples.sh` — 한 번 | exit 1. 첫 TicTacToe cleanup에서 중단. Aggregate의 나머지 sample은 미실행 |
| 변경 범위의 `git diff --check` | 통과 |

## BLOCKERS

다음은 추가 진단이며 **수정 미승인·미구현**이다. 소유 계층은 Framework, 계약은 host relocation
flow §14, 분류 제안은 **B 기존 결함**이다. Timeout 증가, sample assertion 완화, 별도 retry로
해결하지 않았다.

| 원인과 근거 | 관찰 결과 | 후속 수정 방향 |
|---|---|---|
| `N/packages/framework/src/runtime/host/index.ts:1888-1893`은 Draining 게시 성공 뒤에도 polling interval을 무조건 기다린다. 기본값은 `contracts/Locations/Options.ts:27`의 1,000ms다. | TicTacToe PID 86399/86415/86433/86447 모두 SIGINT +3~4ms에 `publishHostDraining()`에 진입하고 반환하기 전에 500ms cleanup SIGKILL을 받았다. Spot teardown에는 아직 도달하지 않았다. | Shutdown 소유 경로에서 게시 후 시간 대기의 계약 근거를 검토하고 불필요한 대기를 제거한다. Grace나 sample polling 설정을 바꾸지 않는다. |
| `spots/index.ts:1102-1107`은 `requestDrainClose()` 후 collection이 비기를 기다리지만 `spot-activation-state.ts:376-378,419-421`은 joined Actor가 있으면 close 자체를 시작하지 않는다. | Support PID 90259는 +110ms에 drain에 진입했다. timer가 있는 Spot 6개에 joined Actor 1~2개가 남았고 모두 `canClose=false`였다. ZoneWorld PID 91839의 `zone-sw`에도 joined Actor 1개와 timer가 남았다. | §14의 Actor membership 유지 상태에서 HostShutdown callback → local scope cleanup 계약을 Spot lifecycle 소유자에서 구현한다. Actor가 먼저 떠나야 종료한다는 조건을 Shutdown에 적용하지 않는다. |
| `spot-activation.ts:738-743`은 close를 detached task로 실행하고 `spots/index.ts:1107`은 collection 소멸만 관찰한다. | User `onClosing()`이 throw한 새 contract에서 `ForceStopped/TeardownFailed` 대신 `Stopped`를 반환한다. | Shared Shutdown이 cleanup terminal 오류를 함께 관찰하도록 기존 close 소유자에 결과를 합친다. Callback 오류를 예외 분기로 숨기지 않는다. |
| `host/index.ts:1918-1920`의 force-stop은 signal 없이 같은 Spot drain을 기다린다. `spot-activation.ts:979-982`는 host 잔여 deadline을 `invokeSpotClosing()`에 전달하지 않아 `spot-closing.ts:6`의 30,000ms가 적용된다. | `shutdown({deadlineMs:50})`과 합류한 `app.close()`가 10초 자식 watchdog까지 자연 종료하지 못했다. 새 contract는 실패를 유지한다. | Host의 deadline과 cleanup terminal을 같은 소유 경로로 전달하고 force-stop도 bounded cleanup으로 끝낸다. 새 timer·budget을 추가하지 않는다. |
| `host/index.ts:1781-1785`은 client 전용 membership에도 `setChannelWeight()`를 호출한다. 시동은 `spot-node-runtime-manager.ts:360-364`에서 server channel만 등록한다. `node-raw-mesh-backend.ts:457`이 미등록 channel을 거부한다. | ZoneWorld trace에서 ZoneNode는 `zoneworld.report`, Ops/Gateway는 `zoneworld.zones` 미등록 오류로 force-stop에 진입했다. 그 뒤 occupied Spot drain에서 zone-node-2가 멈춘다. | 실제 local server membership의 소유자를 재사용해 Draining 게시 대상을 정한다. Client membership을 가짜 server로 등록하지 않는다. |

ZoneWorld gateway의 개별 검증 SIGKILL은 후속 trace에서 재현되지 않았다. 추적 실행에서는
Gateway가 +16ms, Ops가 +17ms에 shutdown을 반환했다. Gateway의 최초 실패 원인은 확정하지
않는다. Ops의 main-thread heartbeat blocking 회귀는 실제 binding contract와 B8에서 해결을
확인했지만, B8의 다른 역할 실패 때문에 ZoneWorld full lane 통과를 주장하지 않는다.

교차언어 근거는 Java의 `closeAllAsync()` / Instance `close(HOST_SHUTDOWN)` 및 오류 수집이다.
Node의 occupied Spot gate·detached close 오류 누락은 이 cleanup 구조와 다르다. 게시 후 대기와
client-only membership 문제의 다른 언어 전체 대조는 후속 승인 진단에서 추가 확인이 필요하다.

최종 workspace 검사에서는 동시 작업 중인 `core/src/runtime/sockets/router/router_admission.cpp`
의 conflict marker 때문에 repository 전체 `git diff --check`가 실패했다. 이 파일은 이번 작업의
변경 범위가 아니며 수정하지 않았다. Node와 이 summary의 diff 검사는 통과했다.

## 증거 위치

전체 명령 로그와 실패 sample의 flow/file log는
[`zlink-work/c016/logs/fix-node-teardown/`](../../../zlink-work/c016/logs/fix-node-teardown/)에 보존했다.

- `verified-individual-results.txt`, `verified-sample-*.log`: 개별 sample 결과.
- `gate-results.txt`, `npm-test.log`, `aggregate.log`, `aggregate-result.txt`: 최종 gate 결과.
- `nestjs-shutdown.log`, `stream-heartbeat-shutdown.log`, `runner-teardown.log`: 회귀 결과.
- `trace-summary.json`, `trace/*.jsonl`: 추가 조사 시 SIGINT 이후 메서드 진입·반환과 Spot 상태.
- `sample-evidence/`: 개별 실패·aggregate·추적 실행의 원래 role log와 flow log 복사본.

TicTacToe의 기존 flow는 play 160건/API 16건 모두 `succeeded`였다. SupportChat의 오류 flow도
제외하지 않고 보존했다. 추가 trace는 이 로그를 먼저 읽은 뒤 미완료 teardown transition을
확인하기 위해 사용했다. 추적용 preload는 조사 후 삭제했으며 runtime에 임시 logging은 남기지
않았다. 추적 실행의 시간은 성능 측정값이 아니다.
