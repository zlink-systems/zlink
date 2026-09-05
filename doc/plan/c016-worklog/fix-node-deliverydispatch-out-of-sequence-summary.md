# Node DeliveryDispatch 순서 오류 조사

## 판정과 증거

제공된 실패는 `DeliveryStatusNotify`가 전송 중 재정렬됐다는 증거가 아니다. 성공 배송
`delivery-success`의 서버 evidence 자체가 `Assigned → Reassigned → Failed`다.
Client가 기대하는 두 번째 상태는 `Accepted`이므로 이 업무 상태 전이가 sequence assertion을
실패시킬 수 있다. Courier 제안 또는 결정이 제시간에 처리되지 않은 원인은 아직 확정하지 못했다.
추가 재현과 독립 probe에서 host wall clock의 약 ±5초 변동을 확인했다. Deadline·lease를
사용하는 실행이므로 이 환경에서 관찰한 실패를 rebuild8 commit의 회귀로 단정할 수 없다.
Runtime 수정과 원인 commit 확정은 하지 않았다.

- 실패 run: `/dev/shm/zlink-tmp-node/zlink-deliverydispatch.ts-nkocLM`.
  `work/deliverydispatch-evidence.jsonl:1`의 Assigned 시각은 `1788596397141`,
  `:2` Reassigned는 `1788596397950`(+809 ms), `:3` Failed는
  `1788596398752`(+802 ms)다. `work/delivery-offers.json`의 최종 상태는 attempt 2 / Failed다.
- `logs/courier-node-2.log:2`부터 courier A/B의 bind relay 완료가 확인된다.
  CourierSession bind 응답까지는 성공했다. 원래 run에는 `logs/flow/` 기록과 browser 수신
  payload가 없으므로 offer 발행 이후 어느 hop에서 지연·누락됐는지는 복원할 수 없다.
- Assertion 위치는
  `framework/languages/node/packages/stream-connector/src/Runtime/Calls/ZlinkStreamObservationBuilders.ts:102`다.
  `framework/languages/node/samples/DeliveryDispatch.Ts/Client/deliverydispatch-client-scenario.ts:69`은
  성공 배송에 Assigned / Accepted / PickedUp / Delivered를 요구한다.
  `Operation canceled`는 함께 남은 종료 오류이며 최초 손실 지점을 특정하지 않는다.
- 만료 판단의 소유자는 sample의
  `framework/languages/node/samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-worker.ts:111`이다.
  `:76`에서 700 ms deadline을 설정한 다음 `:80`에서 offer를 제출한다.
  이 위치만으로 sample 결함이라고 판정하거나 timeout을 늘리지 않는다.

## 계층별 조사

| 후보 | 확인 결과 |
| --- | --- |
| D-B113 `f69558af55` | `message_conversion.ts`는 Buffer 목록을 native submit에 동기 전달한다. 새 binding test에서 multipart payload·part 경계·수신 순서가 보존됐다. 이 변경을 원인으로 확인하지 못했다. |
| D-B114 `6c2cb784c0` | `addon_core.cc:2339`의 `napi_make_callback` 변경은 socket readable callback의 async scope를 만든다. 기존 callback-scope test와 새 completion 순서 test가 통과했다. |
| `8159b15752`의 `writeControl` | `managed-stream.ts:157`은 control frame을 만들고 기존 `socket.submit`을 즉시 호출한 뒤 완료를 기다린다. `node-socket-backend-adapter.ts:168` → binding `StreamSocket.send` → `CompletionOwner.submitSend`로 이어지며 control 전용 지연 queue는 없다. 원인으로 확인하지 못했다. |
| Core `599b4a75ef` | source diff를 검토했지만 현재 증거로 인과관계를 확정하지 못했다. Core 파일을 수정하지 않았으며 재현되지 않은 C API 실패를 주장하지 않는다. |
| Browser sequence tracking | 성공 배송에서 서버가 실제로 다른 상태 전이를 기록했다. Client assertion 완화나 재정렬 buffer를 추가하지 않았다. |

Core socket spec `core/doc/spec/core/socket/README.ko.md:1169`는 completion의 resolver queue
append 순서를 보장하며 **일반적인 submit 순서를 보장하지 않는다**. 추가 테스트는 이보다 좁게,
동일 socket에 128건을 제출하고 server가 같은 순서로 reply하는 상황을 검사한다. N-API callback이
그 순서를 바꾸는지 확인하며, 다른 순서로 완료되는 독립 요청에 새 계약을 부여하지 않는다.
SEND 성공의 즉시 admission Promise와 REQUEST의 native completion을 각각 검사한다.

## 재현과 검증

Framework는 설치된 rebuild8 package를 그대로 사용한다. `f20f5cdba0bc…`는 Core commit SHA가
아니라 설치된 `libzlink.so.0.17.0`의 SHA-256이다. Package provenance의 source revision은
`329541bff1d65f53479781e936f445dad1c60409`이며 `dirty: true`다. 패키지를 다시 설치하지 않았다.

기존 message-flow를 수집하도록 `/tmp/node-deliverydispatch-flow.cjs`에서 OTel provider를
연결했다. Sample은 이미 `messageFlow('normal')`을 설정하지만 provider가 없어 원래 flow
directory가 비어 있었다. 임시 preload는 repository runtime을 바꾸지 않는다. Browser 수신
관찰도 Playwright WebSocket event를 사용한다.

- 첫 재현: exit 0, `/tmp/node-deliverydispatch-repro.log`.
  보존 run은 `/dev/shm/zlink-tmp-node/zlink-deliverydispatch.ts-Q7PJyi`다.
  Courier actor는 node 1에 배치됐다. Courier-node-1 flow `:25–29`에서 offer 수신·발행·완료,
  `:33–38`에서 decision 처리가 보인다. Dispatch flow `:8–12`에 같은 decision flow
  `01a070ad-88c9-706e-97f7-d77c3e0df0e3`의 결과가 도착했다.
  성공 배송 Assigned → Accepted는 35 ms다.
- 진단 도구 실행 오류 1건: `/tmp/node-deliverydispatch-repro-2.log`.
  Playwright를 browser path 설정 전에 로드한 임시 preload 오류다. 제품 실패나 성공으로
  집계하지 않는다. Preload의 초기화 순서를 바로잡았다.
- `npm run build` (`bindings/node`): exit 0.
- 새 binding test: local package 2/2, 설치된 Framework binding package 2/2.
  `/tmp/node-deliverydispatch-binding-order.log`, `/tmp/node-deliverydispatch-installed-order.log`.
- 관련 binding tests: 11/11, `/tmp/node-deliverydispatch-binding-focused.log`.
- 전체 Framework `npm test`: exit 1, announced/completed 1591/1591, 실패 1건.
  `test/contract/sample-regression.test.js:2306`이 실행한 전체 samples의 SupportChat browser
  self-check가 실패했다. 해당 실행의 DeliveryDispatch는 통과했다.
  `/tmp/node-deliverydispatch-framework-test.log:7612`,
  `/dev/shm/zlink-tmp-node/zlink-supportchat.ts-D0qN6u/logs/browser-client.log`를 보존했다.
  이 gate는 node lock만 획득했으며 내부 sample 실행은 별도의 sample lock을 획득하지 않았다.
  따라서 요청된 sample 직렬 실행 조건까지 충족한 clean gate로 보고하지 않는다.
- 별도 `bash samples/run_samples.sh`: exit 1. TicTacToe/Bingo 통과 뒤 DeliveryDispatch startup에서
  `deliverydispatch.dispatch` descriptor claim의 `rejectedConflict`로 실패했다.
  `/tmp/node-deliverydispatch-all-samples.log:91`,
  `/dev/shm/zlink-tmp-node/zlink-deliverydispatch.ts-5PDsXL/logs/dispatch.log:1`.
  Browser 시작 전 실패이므로 원래 sequence 오류와 구분한다.
- Binding 전체 test 파일 순회: 정상 실행된 test 134건 통과(107 + 27).
  첫 순회는 `send_completion_boundary.test.js`가 addon을 직접 로드하며
  `libzlink.so.0`을 찾지 못해 중단됐다. 동일한 rebuild8 prebuild directory를
  `LD_LIBRARY_PATH`에 지정하고 이 파일부터 남은 파일만 실행해 exit 0을 확인했다.
  `/tmp/node-deliverydispatch-binding-all.log`, `/tmp/node-deliverydispatch-binding-remaining.log`.
- 세 번째 traced DeliveryDispatch: exit 0, `/tmp/node-deliverydispatch-traced-3.log`.
  Run은 `/dev/shm/zlink-tmp-node/zlink-deliverydispatch.ts-HKfT8Z`이며 courier A는 node 1,
  B는 node 2에 배치됐다. Server flow 315건에서 인접 record의 wall-clock 경과와 monotonic
  경과가 50 ms 이상 달라지는 구간은 없었다.
  직접 재현 3회는 성공 / 후보 소진 단계 timeout / 성공이다. 3회 모두 exit 0은 아니다.
  전체 `npm test` 내부의 DeliveryDispatch도 별도로 통과했지만 이 결과를 직접 재현 실패와
  교체하여 3/3 성공으로 집계하지 않는다.

모든 Framework 실행은 `TMPDIR=/dev/shm/zlink-tmp-node`, `ZLINK_LIBRARY_PATH` 해제,
`/tmp/zlink-node-gate.lock`을 사용한다. 직접 실행한 sample은 `/tmp/zlink-samples-gate.lock`도 획득한다.
Addon source 변경은 없으며 `npm run build`는 이 repository에서 TypeScript와 test artifact를
생성한다. 설치된 Framework package의 addon은 교체하지 않았다. 향후 addon 수정이 필요하면
감독이 repackaging해야 한다.

## 추가 실패 transition과 시계 증거

`/dev/shm/zlink-tmp-node/zlink-deliverydispatch.ts-pwIwDy`는 성공·재배정 self-check를 통과한 뒤
후보 소진 흐름에서 기다리다가 실패했다. `/tmp/node-deliverydispatch-traced-2.log`에 exit 1을
보존했다. 이 run의 `browser-websocket.jsonl`에는 수신 payload와 관찰 순서를 기록했다.

- 성공 배송은 customer-gateway flow sequence 8/13/18/23의 status send가 browser sequence
  7/10/11/12에 같은 순서로 도착했다. 재배정도 flow 32/37/42/47/52 → browser
  15/23/26/27/28로 대응한다. 수신 status의 `occurredAtUnixMs`와 evidence의 값도 일치한다.
- 후보 소진의 courier A offer는 courier-node-1 flow `:41–43`에서 received/admitted/dispatched까지
  진행했지만 `:44`에서 `ZLinkFrameworkException: Bound session send route is not connected.`로
  실패했다. Flow ID는 `01a070ba-791d-7711-a14c-93d3baeb05d7`다. 이 offer는 browser에 도착하지
  않았으며 courier B의 다음 offer만 browser sequence 63에 도착했다. Client는 A offer를
  기다리는 단계에서 진행하지 못했다.
- 오류 문구는 `runtime/streams/native-fallback-bound-session.ts:136`의 결과 검사 →
  `runtime/messaging/submission-result.ts:38`에서 만들어진다. 이 결과만으로 어느 native
  route가 왜 연결 불가 상태가 됐는지는 확정하지 않는다.
- Browser sequence 32의 `Date.now()`는 `1788597664030`, 뒤의 sequence 33은
  `1788597659231`이다. 관찰 순서는 증가했지만 wall clock은 **4799 ms 후퇴**했다.
- 패키지를 import하지 않은 별도 Node probe `/tmp/node-deliverydispatch-clock.jsonl`에서도
  150번의 100 ms 관찰 중 8번의 큰 변화를 확인했다. Index 27은 wall **+5148 ms** /
  monotonic **+100.223 ms**, index 30은 wall **−5442 ms** / monotonic **+100.466 ms**다.
  `doc/plan/c016-worklog/fix-node-clientserver-readiness-cap-summary.md`의 독립 측정도 같은
  현상을 보고한다. 실제 OS 시각을 변경한 외부 원인은 확인하지 않았다.

Sample deadline은 `dispatch-worker.ts:76/:111`의 `Date.now()`를 사용하며 이 코드는
`b741d64fde0`에서 유래한다. Node STREAM liveness도 `stream-session-runtime.ts:90`에서
`Date.now()`를 사용한다(`a76571fd609`). 둘 다 D-B113/D-B114보다 이전 코드다.
Java server STREAM은 `System.nanoTime()`, .NET server STREAM은 `TimeProvider.GetTimestamp()`로
경과 시간을 잰다. 이는 Node의 시계 변화 취약성을 조사할 근거지만 이 run의 native route
상실 원인을 확정하는 증거는 아니다. Timeout 연장이나 새 timer를 추가하지 않았다.

## 교차언어 대조

| 흐름 | Node | Java | .NET |
| --- | --- | --- | --- |
| 성공 배송 | Assigned → Accepted → PickedUp → Delivered | 동일 | 동일 |
| 재배정 | Assigned → Reassigned → Accepted → PickedUp → Delivered | 동일 | Assigned → Reassigned → Accepted → Delivered |
| 후보 소진 | Assigned → Reassigned → Failed | 동일 | 동일 |

근거: Node client `:69/:113/:166`, Java
`framework/languages/java/samples/java/DeliveryDispatch/Client/src/main/java/systems/zlink/samples/deliverydispatch/client/DeliveryDispatchClientScenario.java:64/:124/:194`,
.NET `framework/languages/dotnet/samples/DeliveryDispatch/Client/DeliveryDispatchClientScenario.cs:68/:135/:211`.
성공 배송의 assertion은 같지만 재배정 assertion까지 모두 같다고 할 수는 없다. 다른 언어는 수정하지 않았다.

소유 계층: 업무 상태·deadline은 sample DispatchWorker, STREAM 송수신은 Core, completion 변환은 Node binding, sequence 검증은 connector.

Spec 조항: DeliveryDispatch §2.1/§9, STREAM session §2/§4 (`04-session/01-stream-session.ko.md`), Node binding “Pull completion 공개 계약”, Core socket completion queue `:1169`.

교차언어 대조: 성공 배송 순서는 Node/Java/.NET 동일; 재배정에서 Node/Java만 PickedUp 포함. Runtime parity 수정 없음.

변경 분류: Runtime A/B/C/D 판정 보류 — 원인 미확정. 변경은 binding 진단 회귀 test뿐이다.

수정 전/후 규칙 수: Runtime 규칙 수 변화 0, 추가 state·timer·retry·정렬 규칙 0.

## 변경 파일

- `bindings/node/tests/completion_order.test.ts`
- `bindings/node/dist-tools/tests/completion_order.test.js` (`npm run build` 생성)
- 이 보고서

## BLOCKERS

- Host wall clock이 약 ±5초 변한다. Deadline·lease를 사용하는 여러 계층의 결과를 Core 또는
  binding commit의 회귀로 판정하기 전에 환경의 시계 변동 원인을 제거해야 한다.
- 원래 실패 run의 message-flow와 browser payload가 없어 원래 courier 경로의 최초 실패
  transition을 확정하지 못했다. 추가 run에서는 bound-session send 실패까지 좁혔으나 그
  연결 불가 결과의 원인은 미확정이다. Core 결함을 재현한 C API sequence도 아직 없다.
- DeliveryDispatch 3회 exit 0, 전체 samples exit 0, Framework `npm test` 0 fail은 충족하지 못했다.
  범위 밖의 startup conflict·SupportChat 실패를 수정하거나 assertion을 완화하지 않았다.
- Runtime 수정으로 해결됐다는 판정을 내릴 근거가 없어 변경하지 않았다. 보호 문서, Core,
  다른 언어와 기존 사용자 변경은 수정하지 않았고 commit하지 않았다.

보고서의 코드·evidence 부합 및 문체를 독립 검토했다. 확인된 line reference와 문체 수정을
반영했으며, 추가 시계·실패 transition 증거도 다시 검토했다. `git diff --check`는 통과했다.
