# Node ZoneWorld Ops 점검 상태 보고 수정

## 결과와 범위

Node ZoneWorld는 점검 상태를 적용한 뒤 `ReportNodeStatusMsg` 제출을 완료하고 응답한다.
상태 보고 payload 생성은 기존 `OpsReportAdapter`가 소유하며, 시작·주기 보고도 같은
메서드를 사용한다. Ops와 shared UI는 기존 `NodeStatusNotify` 경로로 상태를 반영한다.
5초 보고 주기, 15초 report TTL, browser assertion의 5,000ms 제한은 그대로다.

요청한 전체 성공 조건은 충족하지 못했다. 개별 ZoneWorld는 2회 성공·1회 실패이며,
전체 sample과 `npm test`에도 아래 BLOCKERS가 남았다.

Framework runtime, shared UI, Core, binding, 다른 언어와 보호 문서는 수정하지 않았다.
Commit은 수행하지 않았다. 작업 시작 때 존재한 변경과 다른 작업의 변경은 보존했다.

## 상관관계와 원인

기존 실패 증거는 `/tmp/zlink-node-d095-3/zoneworld-failure/`에서 읽었다.
추가 실행 로그와 임시 관측 preload는 `/tmp/zlink-node-zoneworld-ops/`에 보존한다.
Sample의 `messageFlow('normal')`에 OTel provider를 연결했고, Playwright WebSocket
event에서 frame을 저장했다. 임시 관측 코드는 저장소 변경에 포함하지 않는다.

| 수정 전 실행 | 결과 | 실행 디렉터리 |
|---|---|---|
| `repro1.log` | exit 1, `Restore` assertion 실패 | `/dev/shm/zlink-tmp-node/zlink-zoneworld-KAA80r` |
| `repro2.log` | exit 0, browser 3/3 | `/dev/shm/zlink-tmp-node/zlink-zoneworld-16xLhm` |

첫 실행은 preload가 configuration의 `sample` wrapper를 빠뜨려 server flow provider가
연결되지 않았다. WebSocket 기록은 보존됐다. 두 번째 실행부터 설정 경로를 고쳐
server flow도 수집했다. 첫 실행의 server 측 시각·flow를 추정해 채우지 않는다.

실패 실행의 browser 기록(`browser-52568.jsonl`, 시각은 Unix milliseconds):

| 단계 | 시각 | 요청 송신 이후 |
|---|---:|---:|
| east `SetMaintenanceReq(enabled=true)` 송신 | 1788601213942 | 0ms |
| 성공 응답 수신 | 1788601213956 | 14ms |
| east `NodeStatusNotify(maintenance=true)` 수신 | 1788601218789 | 4,847ms |

요청 flow는 `01a070f0-a3f5-7bbb-a8bd-39f6d70b37d5`, correlation은 `2`다.
뒤늦은 status의 flow는 `01a070f0-b6e1-7a15-89f8-ae457d004f8f`다. 점검 적용이
성공했어도 UI가 기다리는 status는 별도 주기에서 발생했다. 실제 frame은 도착했으므로
이 실행을 Ops→browser 유실로 분류할 근거는 없다. 다만 DOM 반영의 정확한 시각은
수집하지 않았으므로 5초 경계에서의 렌더·assertion 관측 순서를 단정하지 않는다.

통과 실행의 같은 경로(`browser-57198.jsonl`, `logs/ops.flow.jsonl`):

| 단계 | 시각 | 근거 |
|---|---:|---|
| 점검 요청 송신 | 1788601353244 | browser frame |
| Ops 점검 성공 reply 제출 | 1788601353253 | Ops flow line 307 |
| 다음 east report를 Ops가 수신 | 1788601358035 | Ops flow line 314 |
| 같은 report에서 `NodeStatusNotify` 제출 | 1788601358036 | Ops flow line 318, session `0000000a` |
| browser가 `maintenance=true` 수신 | 1788601358037 | browser frame |

마지막 세 단계는 flow `01a070f2-d6d2-798a-a42a-dda2903555f7`로 연결된다.
Ops 수신부터 browser 수신까지 2ms이고, 요청부터 status 수신까지는 4,793ms다.
기존 provider는 이 실행의 ZoneNode outbound report `sent` record를 만들지 않았으므로
그 hop의 제출 완료 시각은 이 표에 포함하지 않았다.

확인한 결함은 수정 전 `framework/languages/node/samples/ZoneWorld/Server/ZoneNode/main.ts:94`
의 주기 전용 report와
`Server/ZoneNode/Infrastructure/ZLink/Handlers/node-channel-handlers.ts:39`의
report 없는 maintenance 적용이다. `Shared/spec.ts:38`의 5,000ms 주기 때문에
변경 직후의 report가 다음 tick까지 지연된다. 관측된 약 4.8초 지연이 고정된 5초
UI 검증 경계와 경쟁했을 가능성이 있으나, 해당 실패의 DOM 관측 순서는 확인하지 못했다.
타이머를 실행하지 않는 회귀 테스트에서도 적용 후 push가 0건인 것을 확인했다.

`git blame`에서 보고 없는 maintenance 변경은 `f638d84246e`, 주기 보고 골격은
`10faa2b98cf`까지 거슬러 올라간다. `8159b15752`와 `9981c9fd6e`는 해당 sample 코드를
바꾸지 않았다. 관측 결과는 heartbeat 종료·throttling·시계 점프를 원인으로 지목하지 않는다.

## 수정과 계약

Ops에서 `SetMaintenanceRes`를 보고 상태를 별도로 합성하는 대안과, ZoneNode가 현재
상태를 즉시 보고하는 대안을 비교했다. 실제 상태를 가진 Node의 report 경로를 재사용해
Ops projection과 UI에 점검 전용 갱신 규칙을 추가하지 않는 후자를 선택했다.

- `Server/ZoneNode/Infrastructure/ZLink/Monitoring/ops-report-adapter.ts:21`:
  `reportNodeStatus()`가 현재 node ID·zone 목록·player count·maintenance를 보고한다.
- `Server/ZoneNode/Infrastructure/ZLink/Handlers/node-channel-handlers.ts:42`:
  대상 확인과 상태 적용 뒤 기존 report 경로의 제출을 기다린다.
- `Server/ZoneNode/main.ts`: 시작·주기 보고가 같은 adapter를 호출한다.
- `test/contract/sample-zoneworld-ops.test.js`: timer tick 없이 enable/disable의
  현재 상태가 Ops registry와 구독자까지 전달되는지, 다른 node가 바뀌지 않는지 검증한다.
- `test/contract/sample-zoneworld-gate.test.js`: 기존 payload 생성 위치 검사를 옮긴
  adapter에 연결하고 시작·주기 보고의 호출도 확인한다. browser assertion은 수정하지 않았다.

소유 계층: Node ZoneWorld sample의 ZoneNode 상태 보고; Framework STREAM runtime 변경 없음.

Spec 조항: ZoneWorld §2.2, §7.4, §9.2와 ZW-E1/E6; transport-liveness §1은 STREAM heartbeat와 service liveness가 별개임을 명시한다.

교차언어 대조: .NET `LocalSpotEventHandler.cs:75`의 `NodeStatusReporter`도 5초 주기 보고이며, `OpsServices.cs:34`는 store 기록과 fanout 뒤 응답한다. 같은 경계 위험이 있고 구조적으로 Node만의 문제가 아님을 확인했다. 이번 요청 범위에 따라 Node만 수정했다.

변경 분류: B — sample의 기존 결함. Core·binding 결정을 sample에서 보상하지 않는다.

수정 전/후 규칙 수: ZoneNode→Ops report의 전송 소유자 2 → 1(`main`의 node report와
adapter의 Spot event report → 기존 adapter). 상태 snapshot 생성 위치는 1 → 1로
유지한다. 새 상태·timer·retry·poller는 없다. Maintenance 응답 전에 보장하는 것은
report의 source-local 제출 완료이며, Ops 처리와 browser 반영은 별도의 완료 경계다.

회귀 테스트는 mock submit에서 Ops handler를 호출해 sample 연결과 payload를 검증한다.
실제 process·WebSocket 간 전달 시간과 화면 반영은 다음 live 검증으로 확인했다.

## 수정 후 연결 기록

`verify1.log`의 실행 디렉터리는 `/dev/shm/zlink-tmp-node/zlink-zoneworld-LIhTmH`다.
flow `01a070fa-a98a-761b-ae68-915fc8300e02`가 점검 요청부터 node report와 browser
push까지 유지됐다. Source 제출 시각은 저장소 밖 preload로 기존 adapter 호출의
시작·완료를 관측한 `logs/east.status.jsonl`에서 확인했다.

| 단계 | 시각(Unix ms) | 근거 |
|---|---:|---|
| browser 점검 요청 송신 | 1788601870731 | `browser-84529.jsonl` |
| node status 제출 완료 | 1788601870733 | `east.status.jsonl:32` |
| Ops report 수신 | 1788601870734 | `ops.flow.jsonl:295` |
| Ops status push 제출 | 1788601870735 | `ops.flow.jsonl:299`, session `0000000a` |
| browser `maintenance=true` 수신 | 1788601870745 | `browser-84529.jsonl` |

요청에서 browser 상태 수신까지 14ms였다. 해당 browser test는 enable, diagnostics와
restore를 포함해 337ms에 통과했다. 한 실행의 관측값이며 지연 상한 보장으로 해석하지 않는다.

## 검증

환경: `framework/languages/node`, `TMPDIR=/dev/shm/zlink-tmp-node`,
`unset ZLINK_LIBRARY_PATH`; Node 실행은 `/tmp/zlink-node-gate.lock`, sample은 추가로
`/tmp/zlink-samples-gate.lock`에 `flock -w7200`을 적용했다.

| 검증 | 결과 | 로그 |
|---|---|---|
| 회귀 테스트, 수정 전 | 0/1, 예상한 즉시 push 부재로 실패 | `regression-before.log` |
| TypeScript build + 관련 sample test | 14/14 통과 | `focused.log` |
| ZoneWorld 1회차 | exit 0, verdict 35/35, browser 3/3 | `verify1.log` |
| ZoneWorld 2회차 | exit 1, ZW-F4 관측 timeout; browser 시작 전 | `verify2.log` |
| ZoneWorld 3회차 | exit 0, verdict 35/35, browser 3/3 | `verify3.log` |
| 전체 sample ×1 | exit 1; 앞선 6개 통과, ZoneWorld ZW-F4 timeout | `samples.log` |
| `npm test` ×1 | exit 1; 1599 announced/completed, 1597 통과·2 실패 | `npm-test.log` |

성공한 개별 실행은 각각 35개 verdict line과 spec의 34개 고유 ID를 모두 포함한다.
ZW-B8이 child lane과 부모 집계에 각각 출력된다. 3회차 실행 디렉터리는
`/dev/shm/zlink-tmp-node/zlink-zoneworld-yNii6M`이며, maintenance browser test는
337ms에 통과했다. 전체 sample은 `TicTacToe.Ts`, `Bingo.Ts`, `DeliveryDispatch.Ts`,
`SupportChat.Ts`, `GameQuest.Ts`, `ShoppingMall.Ts`가 모두 완료됐다.

최종 `git diff --check`는 통과했다. 문서는 독립 리뷰에서 코드 부합·문체를 검토했고,
source-local 제출과 원격 완료의 구별, 실패 인과의 관측 한계에 관한 지적을 반영했다.

## BLOCKERS

2회차 `/dev/shm/zlink-tmp-node/zlink-zoneworld-3OqqSG`는 browser 시작 전에
`Runner/sample-runner.mjs:211`의 `scenario ZW-F4 passed` 대기에서 실패했다.
`Client/special.ts:402`부터 boundary bot 위치와 후속 이동을 기다리는 구간이며,
F1·F3까지 출력됐지만 F4 marker는 나오지 않았다. E1~E6는 통과했다.
따라서 이 실행은 maintenance→Restore 재현이 아니며, 요청한 ZoneWorld 3회 전체
성공 조건을 충족하지 못했다. 개별 실행은 2회 성공·1회 실패다.
별도 bot 관측 실패의 root cause는 확정하지 않았다.
해당 fixture·assertion·timeout은 변경하지 않았다.

전체 sample 실행의 ZoneWorld도 같은 F4 대기에서 실패했다
(`/dev/shm/zlink-tmp-node/zlink-zoneworld-ut3jaA`). 진단 preload가 있던 개별 1·3회는
통과했고, 없던 개별 2회와 전체 sample은 실패했다. 이 차이를 무관한 간헐 실패로
단정하지 않는다.

F4 client는 `Client/special.ts:394`에서 `NodeIds.east`(`zone-node-2`)를 고정해
점검한다. 실패 실행의 `zone-node-1.log:24`는 `zone-ne,zone-nw`,
`zone-node-2.log:26`은 `zone-se,zone-sw` 배치를 기록한다. Bot-ne-x는
점검되지 않은 node-1의 north zone 사이를 계속 이동한다
(`zone-node-1.log:158–159,180–181`). 실제 owner를 찾아야 한다는 ZoneWorld §3·§9의
규칙과 다른 가정이다. 이 가정과 F4의 전체 60초 대기 실패 사이의 정확한 시간 관계는
추가 조사가 필요하다. 감독에게 F4의 진단·수정을 이번 범위에 포함할지 확인을 요청했다.

`npm test`의 실패:

- `sample-regression.test.js`, `node run_samples.sh executes every sample self-check`:
  내부 전체 sample 실행도 동일한 ZW-F4 timeout으로 실패했다. 상위 assertion에는 cleanup
  SIGKILL 정규식 불일치로 표시되지만, 포함된 실제 sample 오류는 F4 marker timeout이다
  (`npm-test.log:8121`).
- `stream-actor-bind-replay.test.js:95`, `STREAM actor bind: route absent for the whole
  deadline is Unavailable with no ingress`: `every attempt uses the whole remaining
  original deadline` assertion 실패(`npm-test.log:9669`). Fixture는 `:35`에서
  `Date.now()`를 기록하지만, 이번 작업은 이 test나 Framework deadline runtime을
  수정하지 않았다. 수치와 원인은 추가 진단이 필요하며 시계 점프로 분류하지 않는다.

변경 없이 전체 gate를 다시 실행하지 않았다. F4의 owner 가정과 별도 STREAM deadline
assertion 실패가 남아 있으므로, 이 수정본을 전체 gate가 통과한 상태로 보고하지 않는다.
