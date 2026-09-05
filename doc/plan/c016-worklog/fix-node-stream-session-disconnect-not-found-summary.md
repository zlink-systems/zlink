# Node STREAM session의 NOT_FOUND 종료 처리

이 기록은 감독이 Node gateway 종료 수정의 범위, 계약 근거와 검증 결과를 확인하기 위한 작업 기록이다.
`disconnect_rid`의 typed NOT_FOUND를 이미 완료된 물리 종료로 처리한다. 다른 실패는 원래 오류를 유지한다.

## 원인과 처리 순서

1. 기존 실패 로그 `/dev/shm/zlink-tmp-node/zlink-zoneworld-I8r3Nz/logs/gateway.log:7-24`에서
   `ConfigError`, `code/result=706`, `nativeErrno=2`를 확인했다. 원래 gate 기록은
   `/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/gate13-node-samples.log`다.
2. 수정 전 `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:2018`은
   STREAM submit의 `NotConnected`·`NotFound`·`Terminated` 이후 `disconnectRid`를 직접 호출했다.
   Core에서 RID가 이미 사라졌으면 이 호출의 `ConfigError(706)`가 원래 submit terminal을 덮어썼다.
3. `service-stateful-runtime.ts:3633`의 remote command 36 수신 경로는 typed backend submit terminal을
   session에 귀속시킨다. 바뀌어 전달된 ConfigError는 이 경계에 해당하지 않아 `handleIngress`와 mesh pump 밖으로 전파됐다.
4. 지시된 수정 범위를 B(기존 결함)로 진단하고, 작업 요청의 명시적 수정 지시에 따라 구현했다.
   기존 `wrapSocket().disconnectPeer` 구현을 `disconnectStreamPeer`로 옮겨 raw delivery와 일반 session close가 공유한다.
5. `isBindingNotFound`의 기존 판별식(`ConfigError`이며 `ConfigResult.NotFound`)만 재사용한다.
   숫자 706이나 errno 2만으로 오류를 삼키지 않는다. 성공과 NOT_FOUND 이후 raw delivery는 기존 monitor 관찰로,
   explicit close는 `stream-session-runtime.ts:418-425`의 기존 `queueDisconnect` 호출로 session 정리를 진행한다.
   다른 disconnect 실패는 동일한 오류 객체로 전파하고, raw delivery의 원래 submit terminal도 보존한다.

## 계약과 소유권

- 소유 계층: Node Framework의 STREAM disconnect 어댑터가 binding 결과를 종료 의미로 변환한다. Core가 물리 RID를 소유하고 기존 session runtime이 callback·로컬 Actor binding 정리를 소유한다.
- spec 조항: `core/doc/spec/core/socket/README.ko.md:883-906`의 `zlink_disconnect_rid`, 같은 문서 `:1321`의 routing ID 종료 검증 계약, `framework/doc/framework/common/spec/server/04-session/01-stream-session.ko.md` §4.1·§7·§8.
- 교차언어 대조: .NET과 Java의 물리 disconnect wrapper에는 typed NOT_FOUND 정상화가 없다. Node의 직접 예외 전파를 수정하며 다른 언어의 상태·오류 처리 차이는 아래에 남긴다.
- 변경 분류: **B — 기존 결함**. Core의 대상 부재 결과를 Framework의 완료된 close로 해석한다. Core의 STREAM completion timing에 대한 보상은 없다.

`bindings/doc/spec/node/README.ko.md:561-562`는 Core result 도메인을 보존하는 typed error를 요구한다.
이 문서에는 706의 숫자 매핑을 직접 적은 표가 없으므로 설치된 package의
`dist/zlink/contracts/errors/results.js:98`(`ConfigResult.NotFound=706`),
`dist/zlink/runtime/errors/error_mapping.js:130`(`ConfigError`)과 실제 gateway 로그를 함께 대조했다.

수정 전/후 규칙 수: **STREAM disconnect 결과 처리 위치 2 → 1**. raw service와 socket adapter의 독립 호출을 하나의 처리 함수로 합쳤다.
기존 session 상태·monitor·submit terminal 소유자는 유지하며 상태, timer, retry, poller를 추가하지 않았다.
호출부마다 NOT_FOUND catch를 두는 대안은 같은 결과 해석을 복제하므로 채택하지 않았다.

## 교차언어와 최근 변경 대조

| 대상 | 코드 근거 | 확인 결과 |
|---|---|---|
| .NET bound session | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Streams/ZLinkBoundSessionService.cs:47`, `Runtime/Host/ZLinkActorBoundSessionCoordinator.cs:1435` | actor bound close를 기존 coordinator와 mesh node에 위임한다. 이 경로 자체에 NOT_FOUND 완료 처리는 없다. |
| .NET 물리 STREAM | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendStreamSocketWrapper.cs:169`, `Runtime/Streams/ZLinkStreamNodeRuntime.cs:437` | wrapper는 `DisconnectRid`를 직접 호출한다. receive 실패 경로는 session을 먼저 disconnected로 표시하고 disconnect 예외를 error sink에 기록한다. Node raw delivery와 예외 경계가 다르다. |
| Java 물리 STREAM | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java:170` | state lane 안에서 `disconnectRid`를 직접 호출하며 typed NOT_FOUND 정상화는 없다. |
| Java 정리 | 같은 Java source root의 `runtime/streams/ZLinkStreamRuntime.java:716`, `:874`, `:1564`, `runtime/actors/ZLinkSessionActorsRuntime.java:379-389` | replacement close는 disconnect 실패를 기록한다. malformed receive는 session state를 먼저 제거한다. disconnect stage는 notification 뒤 callback을 실행하고 `whenComplete`에서 context를 제거한다. Actor binding 정리는 `ZLinkSessionActorsRuntime`의 `whenComplete`가 소유한다. |
| `8159b15752` | `stream-session-runtime.ts`의 heartbeat/control 변경 | binding async terminal을 기다리는 기존 변경이다. 이번 수정은 이 terminal 대기 경로를 유지한다. |
| `9981c9fd6e` | 같은 파일의 liveness clock 변경 | `performance.now()`로 duration을 측정한다. 이번 수정은 clock·deadline·timeout을 변경하지 않는다. |

.NET·Java가 같은 상황에서 프로세스를 종료하는지는 재현하지 않았다. typed NOT_FOUND 정상화가 없는 사실과
확인한 cleanup 경계만 보고하며 다른 언어 파일은 수정하지 않았다.

## 변경 파일과 회귀 검증 범위

- `framework/languages/node/packages/framework/src/runtime/backend/node/node-socket-backend-adapter.ts`: 기존 disconnect 구현 이동과 typed NOT_FOUND 완료 처리.
- `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts`: raw delivery가 같은 disconnect owner를 사용한다.
- `framework/languages/node/test/contract/raw-service-mesh.test.js`: 정상 close와 RID 부재 close의 session callback·로컬 binding 정리 비교, remote command 36 ingress 지속, explicit close, 다른 typed 오류·가짜 numeric 오류 보존, 설치된 Core에서 실제 제거된 RID의 delivery.
- 이 작업 기록.

회귀 fixture는 production raw session service, `ServiceStatefulRuntime` ingress, completion table,
`ZLinkStreamSessionRuntime`과 `ZLinkStreamBindingRuntime`을 사용한다. remote bind reply와 monitor 통지는 fixture에서 제공한다.
정리 검증은 실제 session runtime의 제거 callback 및 로컬 Actor binding registry를 대상으로 한다.
Core RID 제거 회귀는 실제 TCP 연결과 설치된 binding의 `disconnectRid`를 사용한다.

## 실행 환경과 결과

- branch: `main`. commit·push 없음.
- `TMPDIR=/dev/shm/zlink-tmp-node`, `ZLINK_LIBRARY_PATH` unset.
- Node 검증: `flock -w7200 /tmp/zlink-node-gate.lock`. Sample은 추가로 `flock -w7200 /tmp/zlink-samples-gate.lock`을 사용한다.
- 설치된 library 파일의 `sha256sum`과 provenance를 대조했다. Core provenance: runtime SHA-256 `083588b48faaf5e1e640961802cfd94847396ea52770b10c2b94216258b79dce`, source `bf28780d5147456c9a7871fb89acd51fb3c40d17`(dirty), local rebuild13.
- 기존 사용자 변경인 `bindings/node/provenance/core-package-provenance.json`, completion-order test와 untracked 작업 디렉터리는 수정하지 않았다.
- ZoneWorld의 기존 `.messageFlow('normal')` 설정을 유지하고 `--keep-run-dir`로 독립 실행 로그를 보존한다.

| 검증 | 결과 | 로그(`/dev/shm/zlink-tmp-node/` 아래) |
|---|---|---|
| `npm run build` | exit 0 | `disconnect-not-found-build.log` |
| `stream-runtime`, `stream-session-runtime`, `raw-service-mesh` | 최종 exit 0, 217/217 통과(157 + 53 + 7) | `disconnect-not-found-focused-final.log` |
| 실제 Core RID 제거 포함 raw 회귀 | exit 0, 7/7 통과 | `disconnect-not-found-native-focused.log` |
| ZoneWorld 1 | exit 0, 34개 verdict 모두 passed | `disconnect-not-found-zoneworld-1.log` |
| ZoneWorld 2 | exit 0, 34개 verdict 모두 passed | `disconnect-not-found-zoneworld-2.log` |
| ZoneWorld 3 | exit 0, 34개 verdict 모두 passed | `disconnect-not-found-zoneworld-3.log` |
| `bash samples/run_samples.sh` 1회 | exit 0, 7/7 sample 완료 | `disconnect-not-found-samples.log` |
| `npm test` 1회 | exit 1, 1609/1611 통과; announced=completed=1611 | `disconnect-not-found-npm-test.log` |

독립 ZoneWorld run 디렉터리는 `/dev/shm/zlink-tmp-node/zlink-zoneworld-XUpC1e`,
`zlink-zoneworld-xAcaZw`, `zlink-zoneworld-OX7svA`다. 각 gateway 로그에 기존 `disconnect_rid failed` 크래시가 없다.

## BLOCKERS

- **남은 실패:** `test/contract/stream-actor-bind-replay.test.js:109`의
  `STREAM actor bind: route absent for the whole deadline is Unavailable with no ingress`.
  `:95`의 `every attempt uses the whole remaining original deadline` assertion이 실패했다
  (`Math.abs(attempt.at + attempt.timeoutMs - deadline) <= 1`). 해당 파일은 4/5 통과했다.
  사용자 지시에 따라 별도 Core 조사 항목으로 분리하고 수정하거나 재실행하지 않았다.
  이 기록은 해당 assertion 실패의 원인이 Core라고 단정하지 않는다.
- **전체 gate 기록:** `npm test`는 지시대로 한 번 실행했다. build·typecheck·lint는 통과했다.
  위 replay 실패 외에 새 native 회귀의 `zlink.Context is not a constructor` fixture 오류가 있었다.
  공개 `createContext()`·`createStreamSocket()`을 사용하고, 기존 Framework factory와 같은
  `StreamRecvMode.Packet` 준비를 적용한 뒤 focused 테스트 217/217이 통과했다.
  전체 gate를 다시 실행하지 않았으므로 원래 exit 1 기록은 유지한다.
- `npm test` 내부 `sample-regression.test.js`도 `run_samples.sh`를 실행하며 52/52 통과했다.
  표의 sample 7/7 결과는 요청한 독립 `bash samples/run_samples.sh` 실행 결과다.
- 이 수정 범위의 남은 재현 실패는 없다. Core·binding·다른 언어·보호 문서·shared_sample 변경과 commit은 없다.

## 독립 리뷰

문서 원칙 §9에 따라 원칙 준수와 코드 부합을 독립 리뷰했다. 같은 ingress에서 다음 delivery를
검증하도록 fixture를 보강했고, explicit close의 직접 정리 경로와 Java `whenComplete` 소유자 서술을 바로잡았다.
중대한 구현 finding은 없으며, 실제 Core 회귀에서도 monitor 통지는 fixture가 제공한다는 검증 범위를 명시했다.
