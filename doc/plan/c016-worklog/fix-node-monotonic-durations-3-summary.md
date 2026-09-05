# Node duration 시간원 통일 후속 결과

감독이 D-095/B의 남은 범위와 검증 결과를 판정하기 위한 기록이다. 이전 보고서에서
STREAM 병행 작업 때문에 보류했던 duration 호출 10곳을 `performance.now()`로 전환했다.
기존 미커밋 변경은 보존했다. Wire·store·보고용 timestamp는 Unix 시각을 유지한다.
관련 계약과 typecheck는 통과했지만, 전체 `npm test`는 ZoneWorld browser의 maintenance 표시
전이 실패 1건으로 exit 1이다. 전체 gate의 0 fail 조건은 달성하지 못했다.

## 소유권과 변경 분류

- 소유 계층: Framework의 STREAM session liveness와 `ServiceStatefulRuntime`의 Message Follow 보관, durable operation deadline, 로컬 terminal replay cache. Core·binding의 연결 선택·completion·재전송 정책은 변경하지 않았다.
- Spec 조항: `server/02-channel-transport/05-transport-liveness.ko.md` §1의 STREAM heartbeat 분리, `04-session/01-stream-session.ko.md` §1의 session lifecycle, `05-location-relocation/04-relocation-flow.ko.md` §10의 Message Follow 기간·local relay window, `05-location-relocation/01-location-runtime.ko.md` §7의 원래 deadline·최종 결과 보관 계약. 시간원 선택은 감독이 승인한 D-095다. Store의 최종 결과 보관은 Store 시각을 유지하며, 이번 replay 변경은 프로세스 안의 cache에만 적용한다.
- 교차언어 대조: Java `runtime/streams/ZLinkStreamRuntime.java:1450,1605,1606`은 liveness를 `System.nanoTime()`으로 계산한다. `runtime/binding/ZLinkJavaDurableRequest.java:39,63,77,126`도 하나의 monotonic deadline과 상대 timer를 사용한다. Java `ZLinkJavaRawMeshNode.java:7513–7517,7680–7693`의 로컬 terminal retention은 아직 wall clock이다. 이 차이는 구조상 필수가 아닌 같은 시간원 결함의 별도 언어 범위이며, 다른 언어는 수정하지 않았다.
- 변경 분류: B — 이전 진단과 이번 감독 지시가 승인한 기존 경과 시간 계산 결함 수정. Public API·wire·store 계약 적응이나 우회가 아니다.

수정 전/후 규칙 수: 로컬 duration의 시간원 2개(wall clock과 상대 timer의 monotonic clock) →
1개(`performance.now()`와 기존 상대 timer). Replay 보관 시각 필드 1 → 1, 추가 timer·retry 정책·
시간원 자동 판별 분기 0 → 0. Timestamp 계약은 duration 규칙과 구분한다.

기존 deadline owner의 시간원과 보관 필드를 직접 바꾸는 안을 적용했다. Wall clock 보정 상태나
별도 만료 timer를 추가하는 안은 시간 보정·정리 규칙을 추가하므로 채택하지 않았다.
Binding timeout은 정수 밀리초 계약이므로 제출 경계에서 `Math.ceil`로 변환한다. 원래 timeout,
20 ms retry delay 상한, 30,000 ms relay 상한, 5분 replay retention과 만료 비교 연산은 유지한다.

## 호출 지점별 변경

경로 prefix는 `framework/languages/node/packages/framework/src/runtime/`다. 이전 line은
`fix-node-clientserver-readiness-cap-2-summary.md`의 감사 기준이며 현재 line은 이번 수정 후다.

| 이전 file:line | 현재 line | 기간 소유자와 변경 |
|---|---:|---|
| `streams/stream-session-runtime.ts:90` | 90 | 기본 liveness clock의 `now()`를 `performance.now()`로 변경. 기존 heartbeat·idle·replacement callback timer가 같은 clock을 사용한다. |
| `foundation/service-stateful-runtime.ts:1147` | 1147 | Message Follow `expiresAtMs`를 monotonic 시작 시각 + 전달받은 duration으로 생성. duration 0의 기존 처리 유지. |
| `foundation/service-stateful-runtime.ts:1632` | 1632 | STREAM bind의 로컬 end-to-end deadline 생성. pending operation의 상대 timer와 timeout 값 유지. |
| `foundation/service-stateful-runtime.ts:4035` | 4038 | User Spot create/close·Actor create durable request의 로컬 deadline 생성. |
| `foundation/service-stateful-runtime.ts:4081` | 4084 | durable request의 남은 시간을 monotonic으로 차감하고 binding 제출용 정수 밀리초로 변환. |
| `foundation/service-stateful-runtime.ts:4094` | 4097 | 실패 뒤 같은 deadline의 remaining으로 기존 `min(20, remaining)` timer 지연 계산. |
| `foundation/service-stateful-runtime.ts:4762` | 4765 | Message Follow ingress에서 기존 monotonic `expiresAtMs`의 만료 판정. |
| `foundation/service-stateful-runtime.ts:4805` | 4808 | Message Follow drain에서 같은 만료 시각을 사용. |
| `foundation/service-stateful-runtime.ts:4826` | 4829 | relay request에 `max(1, min(30_000, ceil(remaining)))` 제출. 새 deadline이나 budget 없음. |
| `foundation/service-stateful-runtime.ts:5218` | 5221 | replay 만료 비교를 monotonic으로 변경. `:407,3937–3949`에서 기존 cache의 `deadlineUnixMs` 필드를 `replayExpiresAtMs`로 교체하고 최초 admission에서 Unix deadline의 남은 기간 + 기존 5분을 한 번 변환. Lookup과 capacity sweep은 같은 필드·비교 함수를 사용. |

`service-stateful-runtime.ts:3938`의 `Date.now()`는 외부 Unix deadline을 로컬 기간으로 바꾸는
입구다. 이후 replay lookup/sweep에서 wall clock을 다시 읽지 않는다. `:5209,5259`의 기존
Unix deadline → 상대 timer/remaining 변환도 유지한다. Record의 `deadlineUnixMs`, wire fingerprint,
store publication, reported timestamp에는 monotonic 원점을 넣지 않는다.

## 테스트 변경과 근거

경로 prefix는 `framework/languages/node/`다.

| 파일 | 변경·근거 |
|---|---|
| `test/contract/stream-runtime.test.js:1187` | 만료된 retry deadline fixture를 `performance.now() - 1`로 변경. Owner `host/actor-packet-relay.ts:556,607`의 monotonic 입력 계약에 맞춘다. Error sink 1건, typed DeadlineExceeded, 원래 cause assertion은 그대로다. |
| `test/contract/stream-session-runtime.test.js` | 기본 liveness clock으로 ±60,000 ms wall-clock jump 회귀 추가. 최초 ping 뒤 monotonic 4,999 ms에는 유지하고 5,000 ms에는 heartbeat timeout으로 닫는지 검증. Clock 주입 옵션으로 기본 시간원을 우회하지 않는다. |
| `test/m6b/m6b-runtime.contract.ts` | terminal cache capacity fixture의 필드를 owner와 같은 monotonic 만료 시각으로 변경. ±301,000 ms wall-clock jump 중 같은 terminal을 재응답하고, 기존 5분 + 1초 만료 검증은 performance clock을 전진시켜 수행한다. 원래 실행 1회·TimedOut·Busy assertion과 capacity 값 유지. |
| `test/m6b/m6b-user-spot-terminal-replay.contract.ts` | ±60,000 ms wall-clock jump를 첫 실패에 주입. 500 ms deadline에서 40 ms 경과 뒤 다음 제출 budget이 460 ms이고, request bytes와 wire Unix deadline이 바뀌지 않는지 검증. |
| `test/contract/topology-runtime-projection.test.js` | 이번 실행에서 추가 변경 없음. 기존 미커밋 수정의 assertion을 읽고 재검증했다. Relocate preflight는 monotonic `startedAt`과 비교하며 200 ms budget을 유지한다. Shutdown cleanup은 performance clock을 51 ms 전진시켜 원래 50 ms 한도의 ForceStopped/DeadlineExceeded를 검증한다. 공유 shutdown은 최초 보고용 Date 객체의 identity assertion을 유지한다. |

이번 새 변경 파일은 runtime 2개, 테스트 4개와 이 보고서다. Topology·host 등의 이전 미커밋
변경은 그대로 남아 있다. Core, binding, 보호 문서, 다른 언어는 수정하지 않았고 commit하지 않았다.

## 검증 결과

작업 디렉터리는 `framework/languages/node`, `TMPDIR=/dev/shm/zlink-tmp-node`,
`ZLINK_LIBRARY_PATH` 해제다. Build·typecheck·관련 테스트는
`flock -w7200 /tmp/zlink-node-gate.lock`에서 실행했다. 전체 `npm test`는 같은 잠금과
`flock -w7200 /tmp/zlink-samples-gate.lock`을 함께 잡고 한 번 실행했다.
로그 디렉터리는 `/tmp/zlink-node-d095-3/`다.

| 검증 | 결과 | 로그 |
|---|---|---|
| `npm run build` | 통과 | `build.log` |
| `npm run typecheck` | 통과 | `typecheck.log` |
| STREAM runtime/session + topology projection + drain 계약 | 262/262, fail 0 | `focused-final.log` |
| M6B compile + stateful runtime/durable terminal replay 계약 | 121/121, fail 0 | `m6b-build.log`, `m6b-final.log` |
| `git diff --check -- framework/languages/node` | 통과 | 명령 출력 없음 |
| `npm test` 1회 | exit 1. Build/typecheck/lint 통과. TAP announced=1598/completed=1598; 실패 파일은 sample-regression 1개 | `npm-test.log` |
| 전체 gate 안의 STREAM runtime/session·topology projection | 각각 157/157, 53/53, 27/27; fail 0 | `npm-test.log` |
| 전체 gate 안의 실제 sample self-check | sample-regression 51/52, fail 1. ZoneWorld browser maintenance 표시 실패. DeliveryDispatch runner는 성공 종료 후 다음 sample로 진행 | `npm-test.log`, 아래 보존 로그 |

## BLOCKERS

- `npm test`의 남은 실패는 `test/contract/sample-regression.test.js:2306`의 실제 sample 실행이다.
  STREAM retry fixture와 topology projection의 이전 실패는 이번 전체 gate에서 재발하지 않았다.
- 실패 역할·전이: ZoneWorld **Ops browser → zone-node-2 maintenance → Ops browser 표시**.
  공통 browser test `framework/languages/shared_sample/zoneworld/client/tests/live/server.spec.ts:52`는
  east node의 `Maintain` 클릭 뒤 `Restore` 버튼이 보이기를 5,000 ms 기다렸으나 찾지 못했다.
  `operations page applies owner-targeted maintenance and diagnostics`가 실패했고, 같은 browser
  lane의 owner boundary 유지·node loss push 테스트는 통과했다(2 passed / 1 failed).
- Node runner는 `samples/ZoneWorld/Runner/sample-runner.mjs:731`에서 browser exit 1을 보고했다.
  sample-regression의 `:2324`는 stderr를 cleanup SIGKILL 패턴과 대조하다 실패했다. 실제 원인은
  cleanup 강제 종료가 아니라 위 browser assertion이다. 이 assertion이나 5,000 ms 값은 수정하지 않았다.
- 보존된 `zone-node-2.log:284`에는 `maintenance cache updated node=zone-node-2 enabled=true`가 있고,
  `:313`에는 node status 제출이 있다. Browser의 버튼 표시 전이 실패까지는 확인했으나, 해당
  요청의 status 전달·UI 반영 중 정확한 실패 owner는 이번 로그만으로 확정하지 않았다. 이 실행
  디렉터리에는 message-flow 상관 로그가 없으며, 임시 로깅이나 재현 재실행은 하지 않았다.
  이 관측만으로 liveness 시간원이나 Core를 원인으로 판정하지 않는다.
- 실제 sample은 `run_samples.sh`의 `set -e` 순차 실행에서 DeliveryDispatch를 포함한 앞선 sample을
  성공 종료하고 ZoneWorld까지 진행했다. DeliveryDispatch 실패로 분류하지 않는다. 전체 sample
  성공이나 `npm test` green으로 보고하지 않는다.
- 실패 로그는 `/dev/shm/zlink-tmp-node/zlink-zoneworld-L5RbQp/logs/`에 보존되어 있다.
  `shared-browser-playwright.log:13–39`가 실패 assertion과 browser 결과를 기록한다. 해당 browser,
  zone-node-2, ops 로그 사본은 `/tmp/zlink-node-d095-3/zoneworld-failure/`에도 보존했다.

보고서의 코드 부합·산문 원칙 독립 검토는 finding 없음이다. 실행 결과 집계는 원본 TAP과
process exit code로 별도 확인했다. 추가 전체 gate나 sample 재실행은 하지 않았다.
