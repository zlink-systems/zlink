<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Spot actor relocation](config-10-spot-actor-relocation.ko.md) |
[다음: Channel egress routing](config-12-channel-egress-routing.ko.md)
<!-- framework-adapter-nav:end -->

# Config 11 — Flow, metric과 Host maintenance 관측

운영자는 한 application message가 여러 node와 Actor·Spot을 지나는 흐름을 추적하고, connection·request와
relocation의 집계 metric을 확인하며, Host maintenance가 어느 상태까지 진행됐는지 판단해야 한다. 이
정보는 정식 public observability 계약이므로 이 config에서는 flow log, metric reader와 Host status를 직접
검증 근거로 사용한다.

Messaging payload와 lifecycle 결과는 역할 server의 application evidence로 확인한다. Location Store row,
relocation manifest, Core peer table과 private counter는 판정에 사용하지 않는다.

## 1. 확인 범위

- STREAM에서 Actor와 Spot으로 이어지는 flow correlation
- 실패, fanout, timer와 runtime tracing level 변경
- Stream connection, relocation, request와 owner lease metric
- Actor·User Spot handoff, rolling update와 planned maintenance
- Relocate blocker, concurrent call, cancellation과 Shutdown 경쟁

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Global object 위치와 automatic topology를 제공한다. |
| Relocation Store | 1 | `PreserveStateWith` Actor·Spot relocation payload를 보존한다. |
| Session gateway | 1 | Stream Session, Actor binding과 Session relay를 제공한다. |
| Play node | 2~4 | Actor, `SpotWide` User Spot과 Instance Spot factory·adapter를 제공한다. Version·capacity와 maintenance state가 다른 target variant를 구성한다. |
| Order workflow | 2 | Fanout projection과 timer-origin flow를 만든다. Play relocation target에는 참여하지 않는다. |
| E2E client | scenario별 | Stream과 역할 server의 public application endpoint를 호출한다. |

각 host는 정식 message-flow 설정과 언어 표준 metric reader를 사용한다. Flow 검증은 spec에 정의된 field와
public trace record만 파싱한다. Metric은 scenario 시작 직전 snapshot과 동작 완료 뒤 snapshot의 delta로
비교하고 process 누적값을 고정값으로 가정하지 않는다.

## 3. 공통 실행과 판정 방법

Runner는 scenario마다 Store namespace, object ID, flow marker와 metric reader를 새로 만든다. Role health,
public topology status와 Host status가 필요한 시작 상태가 된 뒤 operation을 시작한다. Relocation의 특정
구간을 유지해야 하면 Application factory, adapter 또는 handler callback이 public application signal에서
대기하도록 한다. Framework 내부 state transition을 멈추는 hook은 사용하지 않는다.

Flow와 metric은 이 config의 검증 대상이므로 통과 조건에 사용할 수 있다. 일반 file log 문자열, internal
debug event와 implementation-specific allocation count는 진단 자료로만 사용한다.

## 4. Scenario

### Track A — Process 간 message flow 연결

#### OBS-A1 STREAM에서 Actor와 room Spot까지 같은 flow를 유지한다

우선순위: `P0`

Client action이 Session gateway, Actor와 room Spot을 차례로 지나도 하나의 flow로 검색할 수 있어야 한다.

**검증 질문:** 한 Stream request의 connector·Session·Actor·Spot trace가 같은 flow ID를 가지는가.

- 시작 조건: Client Session이 Player Actor에 bind되어 있고 Actor는 room Spot에 존재한다. 모든 역할의
  tracing level은 `key_transitions`다.
- 절차: Client가 고유 marker의 game action을 Stream으로 한 번 보낸다.
- 검증: Connector outbound, Session inbound, Actor relay와 room Spot dispatch record가 같은 flow ID와
  marker를 가진다. 각 hop의 application handler는 한 번 실행된다.
- 세부 동작: [Flow correlation §5](../spec/27-flow-correlation.ko.md)을 검증한다.

#### OBS-A2 Dispatch 실패 record에도 flow를 남긴다

우선순위: `P0`

Handler가 없는 message도 원래 요청 흐름과 함께 검색할 수 있어야 원인 분석이 가능하다.

**검증 질문:** 등록되지 않은 packet의 public error trace가 원래 flow ID를 포함하는가.

- 시작 조건: Caller tracing이 켜져 있고 target에는 negative packet handler를 등록하지 않는다.
- 절차: Public typed client로 negative packet을 한 번 보내고 이어서 정상 packet을 보낸다.
- 검증: Negative dispatch의 trace에는 caller가 만든 flow ID와 정식 error phase가 있다. 정상 packet도
  독립 flow로 처리되며 두 marker가 섞이지 않는다.
- 세부 동작: [Flow correlation §7](../spec/27-flow-correlation.ko.md)을 검증한다.

#### OBS-A3 Tracing off 구간은 inbound flow를 전파하지 않는다

우선순위: `P1`

Tracing을 끈 node는 flow context를 만들거나 다음 hop으로 복사하지 않는다. 다음 enabled node는 flow가 없는
inbound message에서 새 flow를 시작한다.

**검증 질문:** Enabled→off→enabled 경로에서 마지막 node가 앞선 flow와 다른 새 ID를 사용하는가.

- 시작 조건: Source와 target은 `key_transitions`, 중간 node는 `off`다.
- 절차: 세 node를 지나는 고유 marker의 message를 한 번 보낸다.
- 검증: Source record에는 flow가 있고 off node에는 flow trace가 없다. Target은 source와 다른 새 flow ID를
  만들며 application payload는 정상 처리한다.
- 세부 동작: [Flow correlation §4](../spec/27-flow-correlation.ko.md)을 검증한다.

#### OBS-A4 Fanout branch는 flow를 공유하고 timer는 새 flow를 만든다

우선순위: `P1`

한 publish에서 갈라진 subscriber record는 원래 flow를 공유한다. 반대로 기존 inbound operation이 없는 timer
callback은 새 timer-origin flow를 시작한다.

**검증 질문:** Fanout subscribers는 같은 flow를 받고 timer callback은 별도 timer-origin flow를 가지는가.

- 시작 조건: Order workflow와 subscriber N개가 ready이고 room timer가 등록되어 있다.
- 절차: Workflow handler에서 projection event를 publish하고 별도로 one-shot room timer를 실행한다.
- 검증: N subscriber trace가 publish flow ID를 공유한다. Timer trace는 다른 flow ID와 `origin=timer`를
  가지며 timer handler가 한 번 실행된다.
- 세부 동작: [Flow correlation §4](../spec/27-flow-correlation.ko.md)와
  [§5](../spec/27-flow-correlation.ko.md)을 검증한다.

#### OBS-A5 실행 중 tracing level 변경을 적용한다

우선순위: `P0`

Tracing level 변경은 업무 처리를 멈추지 않고 변경 완료 뒤 시작한 message부터 적용되어야 한다.

**검증 질문:** `key_transitions→off→errors_only→key_transitions` 전환 중 모든 request가 처리되고 각
marker의 trace 범위가 level과 일치하는가.

- 시작 조건: Public diagnostics control을 제공하는 역할 server와 정상·error packet을 준비한다.
- 절차: 각 level 변경 awaitable이 완료된 직후 서로 다른 marker의 정상 또는 error request를 보낸다.
- 검증: 모든 request는 정식 application 결과를 가진다. Off marker에는 flow trace가 없고 errors-only에는
  error record만 있으며 마지막 marker부터 새 flow trace가 다시 생긴다.
- 세부 동작: [Flow correlation §8](../spec/27-flow-correlation.ko.md)을 검증한다.

### Track B — Runtime metric과 실제 사건 대조

#### OBS-B1 Stream connection과 reconnect metric을 확인한다

우선순위: `P0`

Active connection gauge와 reconnect counter는 실제 connector lifecycle과 일치해야 한다.

**검증 질문:** Client 연결·종료·자동 재접속 뒤 metric delta와 current gauge가 실제 connection 수와
일치하는가.

- 시작 조건: Session server와 connector metric reader의 baseline을 저장한다.
- 절차: Client N개를 연결하고 일부를 정상 종료한다. 한 connector의 network를 끊었다가 자동 reconnect가
  완료되도록 한다.
- 검증: Server active connection gauge는 각 단계의 실제 연결 수와 일치한다. Connector reconnect counter는
  해당 자동 재접속 사건만큼 증가하며 label은 spec의 닫힌 값만 사용한다.
- 세부 동작: [Runtime metrics §4](../spec/25-runtime-metrics.ko.md)를 검증한다.

#### OBS-B2 Actor relocation metric을 확인한다

우선순위: `P0`

Relocation counter와 duration은 실제 Actor 이동 terminal과 일치해야 한다. Interruption 목표 초과를
relocation 실패로 바꾸지는 않는다.

**검증 질문:** Actor 이동 한 번이 relocation 완료·duration·interruption metric에 한 번 반영되는가.

- 시작 조건: Actor가 `play-a`에 ready이고 metric baseline을 저장한다.
- 절차: Public Join 또는 Host Relocate로 Actor를 `play-b`에 이동하고 public current Actor location과
  completion callback을 확인한다.
- 검증: Completed counter delta는 1이고 `object_kind=actor`다. Duration과 interruption sample이 각각
  하나 추가되며 public 이동 결과가 성공이면 interruption 시간이 목표를 넘더라도 completed outcome을
  실패로 바꾸지 않는다.
- 세부 동작: [Runtime metrics §5](../spec/25-runtime-metrics.ko.md)을 검증한다.

#### OBS-B3 Publish metric 부재와 owner lease lateness를 확인한다

우선순위: `P1`

Spec은 Logical Multicast와 classic fanout의 target별 publish metric을 제공하지 않는다. Owner lease 갱신
지연은 낮은 cardinality metric으로 제공한다.

**검증 질문:** Publish delivery는 application evidence로만 보이고 Store 지연은 lease lateness metric에
기록되는가.

- 시작 조건: Metric reader baseline과 ready subscribers를 준비한다.
- 절차: Fanout과 Logical Multicast marker를 각각 publish한다. Runner가 Redis 응답을 외부에서 지연시켜
  owner lease renew lateness를 만든다.
- 검증: Subscribers는 marker를 받지만 publish target·receive·drop 전용 metric은 생기지 않는다. Lease
  lateness sample은 증가하고 어떤 metric label에도 flow ID, Actor ID와 Spot ID가 없다.
- 세부 동작: [Runtime metrics §6](../spec/25-runtime-metrics.ko.md)과
  [§7](../spec/25-runtime-metrics.ko.md)을 검증한다.

#### OBS-B4 Metric reader가 없어도 messaging을 처리한다

우선순위: `P1`

Metric reader와 exporter는 Application이 선택하는 수집 경계다. Reader가 없다는 이유로 업무 path가
실패해서는 안 된다.

**검증 질문:** Metric reader를 등록하지 않은 host도 같은 request·send 결과를 제공하는가.

- 시작 조건: 같은 설정의 host A에는 reader를 등록하고 B에는 등록하지 않는다.
- 절차: 두 host에 같은 marker의 request와 send를 각각 100개 실행한다.
- 검증: 두 host의 reply, handler count와 payload 값이 같다. Reader가 없는 B에서 별도 exporter나 evidence
  queue를 요구하지 않는다. Allocation과 clock-read 비용은 benchmark 책임이다.
- 세부 동작: [Runtime metrics §8](../spec/25-runtime-metrics.ko.md)을 검증한다.

### Track C — Host Relocate와 Shutdown을 운영

#### OBS-C1 Relocating host를 신규 placement에서 제외한다

우선순위: `P0`

Relocate를 시작한 Host는 기존 accepted work와 infrastructure를 유지하지만 신규 placement 후보에서는
빠져야 한다. 완료 뒤에는 process를 종료하지 않고 `Relocated` 상태로 남는다.

**검증 질문:** Host status와 placement 결과가 `Serving→Relocating→Relocated` 전이를 함께 반영하는가.

- 시작 조건: `play-a`에 stateful object가 있고 `play-b`는 eligible target이다. Target adapter는 application
  signal에서 restore 완료를 보류한다.
- 절차: `play-a`에 public Relocate를 시작한다. Target restore-held 상태에서 Host status와 신규 object
  placement를 확인한 뒤 restore를 해제한다.
- 검증: Held 구간에 source는 `Relocating`, not-ready, not-accepting이며 신규 object는 source에 배치되지
  않는다. 완료 뒤 source status는 `Relocated`이고 process health endpoint는 유지된다. Host state metric도
  같은 닫힌 state를 반영한다.
- 세부 동작: [Host maintenance §13](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C2 Actor handoff 뒤 bound Session push를 유지한다

우선순위: `P0`

Actor가 다른 node로 이동하면 Framework가 bound Session의 Actor 위치를 갱신한다. Application이 다시 bind하지
않아도 이동한 Actor의 push와 Session relay가 이어져야 한다.

**검증 질문:** Host Relocate 뒤 같은 Session binding으로 Actor push와 relay가 새 owner에서 처리되는가.

- 시작 조건: `play-a` Actor가 Session에 bind되어 있고 pre-relocation push를 client가 받았다.
- 절차: `play-a`를 Relocate하여 Actor가 `play-b`로 이동한 completion을 확인한다. Bind를 다시 호출하지 않고
  client가 relay request를 보내고 Actor가 post-relocation push를 보낸다.
- 검증: Public current Actor location은 `play-b`이고 request handler evidence도 B에 있다. Client는 push를
  한 번 받으며 binding count와 Actor identity는 유지된다.
- 세부 동작: [Host maintenance §8.3](../spec/28-graceful-drain-handoff.ko.md)과
  [Session Actor dispatch §7](../spec/20-session-actor-dispatch.ko.md)을
  검증한다.

#### OBS-C3 User Spot aggregate와 member Actor를 함께 이동한다

우선순위: `P0`

`SpotWide` User Spot은 Spot 상태와 member Actor 상태를 하나의 aggregate로 target에 복원한다. 이동 뒤 같은
global IDs로 message를 보내면 target에서 기존 application state를 이어서 처리해야 한다.

**검증 질문:** User Spot과 모든 member Actor가 identity·generation·state를 유지한 채 target에서
처리되는가.

- 시작 조건: `play-a`에 counter 상태가 있는 User Spot과 member Actor 두 개가 있다. Public refs와 state
  values를 저장한다.
- 절차: Host Relocate를 실행하고 public completion을 기다린다. 같은 SpotId와 ActorIds로 request를 보내고
  refs를 다시 조회한다.
- 검증: 모든 current locations는 `play-b`이며 ObjectGeneration은 이전 ref와 같다. Spot counter와 Actor
  state가 보존되고 각 handler가 target에서 한 번 실행된다. Source `OnClosing(RelocationOut)`과 target
  restore application callbacks도 operation당 정식 횟수로 기록된다.
- 세부 동작: [Host maintenance §8.5](../spec/28-graceful-drain-handoff.ko.md)를
  검증한다.

#### OBS-C4 Shutdown은 relocation 없이 closing callback과 Session close를 수행한다

우선순위: `P1`

Shutdown은 stateful object를 다른 node로 옮기는 operation이 아니다. Local Spot에 closing reason을 알리고
active Session을 정식 close reason으로 종료한 뒤 Host를 멈춘다.

**검증 질문:** Shutdown에서 각 Spot의 `OnClosing(HostShutdown)`과 client close reason이 한 번씩
관찰되는가.

- 시작 조건: Entry·User·Instance Spot과 active Stream Session이 source Host에 있다.
- 절차: Public Shutdown을 호출하고 lifecycle callback, client close와 Host terminal을 기다린다.
- 검증: 각 Spot callback은 `HostShutdown` reason으로 한 번 실행되고 callback 시점에 application state를
  읽을 수 있다. Client는 정식 server-drain close reason을 받고 Host result는 `Stopped/None`이다. Target
  node에 새 object가 생기지 않는다.
- 세부 동작: [Host maintenance §10](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C5 Eligible target이 없으면 source를 유지한다

우선순위: `P1`

Relocation을 받을 compatible target이 없으면 source object를 변경하지 않고 preflight에서 blocked result를
반환해야 한다.

**검증 질문:** Target version·type·capacity 조건을 만족하는 node가 없을 때 source가 계속 request를
처리하는가.

- 시작 조건: Stateful object는 `play-a`에 있고 다른 nodes는 absent, wrong version, missing type 또는 full
  capacity 중 하나다.
- 절차: 각 blocker를 fresh fixture에서 만들고 public Relocate를 호출한다. Terminal 뒤 source object에
  request를 보낸다.
- 검증: Relocate는 정식 `Blocked` outcome과 blocker reason으로 끝난다. Source Host는 Serving이고 public
  location과 generation이 유지되며 follow-up request가 성공한다. Shutdown을 자동 시작하지 않는다.
- 세부 동작: [Host maintenance §6](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C6 Rolling update로 exact 새 version에 이동한다

우선순위: `P0`

Application patch는 새 version target을 먼저 ready로 만든 뒤 source workload를 그 exact version으로
이동한다.

**검증 질문:** `TargetApplicationVersion=N+1` Relocate 뒤 모든 current object가 N+1 target에서
처리되는가.

- 시작 조건: Version N source와 compatible N+1 target이 ready이며 source에 Actor·User Spot·Instance Spot과
  bound Session이 있다.
- 절차: 지속 request와 push 중 RollingUpdate Relocate를 호출한다. Completion 뒤 같은 IDs로 request와
  push를 실행한다.
- 검증: Result는 `Relocated/None`, mode `RollingUpdate`, effective version N+1이다. Current locations와
  handler evidence는 N+1 target을 가리키며 binding과 object generation은 유지된다. Source process는
  Relocated로 유지되고 후속 explicit Shutdown이 Stopped로 끝난다.
- 세부 동작: [Host maintenance §5](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C7 Planned maintenance는 같은 version target을 사용한다

우선순위: `P0`

Node 점검은 Application version을 바꾸지 않고 같은 version의 compatible node로 workload를 옮길 수 있다.

**검증 질문:** PlannedMaintenance가 version N workload를 다른 version N target으로 이동하는가.

- 시작 조건: Source와 target은 모두 version N이며 target에 필요한 type·capacity가 있다.
- 절차: Source stateful object에 accepted request가 있는 상태에서 PlannedMaintenance Relocate를 호출한다.
- 검증: Accepted request는 terminal 결과를 하나 받고 Relocate result의 effective version은 N이다. Current
  object location과 후속 handler evidence는 target을 가리키며 state와 generation이 유지된다.
- 세부 동작: [Host maintenance §5](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C8 Shutdown deadline에서 bounded forced teardown을 수행한다

우선순위: `P1`

Spot closing callback이 끝나지 않아도 Shutdown은 host deadline을 넘겨 무기한 기다리지 않는다.

**검증 질문:** Closing callback을 application gate에서 막으면 Shutdown이 `ForceStopped/DeadlineExceeded`로
끝나는가.

- 시작 조건: Spot `OnClosing`이 entered evidence를 남긴 뒤 application signal을 기다리도록 구성한다.
- 절차: Gate보다 짧은 양수 deadline으로 Shutdown을 호출하고 terminal을 기다린 뒤 gate를 해제한다.
- 검증: Callback이 받은 absolute deadline은 Host deadline과 같다. Host result는
  `ForceStopped/DeadlineExceeded`이고 forced-shutdown metric delta는 1이다. Late callback completion이 Host
  terminal을 바꾸지 않는다.
- 세부 동작: [Host maintenance §10](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C9A Automatic topology는 target ready 뒤 relocation을 시작한다

우선순위: `P0`

Descriptor가 보인다는 사실만으로 target transport가 ready인 것은 아니다. Relocate는 exact target이 public
topology status에서 ready가 된 뒤 workload를 이동해야 한다.

**검증 질문:** Target이 not-ready인 동안 source를 유지하고 ready가 된 뒤 relocation을 완료하는가.

- 시작 조건: Compatible target process는 시작했지만 runner가 source-target network를 차단하여 public
  status가 not-ready다.
- 절차: Relocate를 시작하고 pending 상태에서 source follow-up request를 보낸다. Network를 복구하여 target
  ready를 확인한다.
- 검증: Not-ready 구간에 source request가 정상 처리되고 current location은 source다. Target ready 뒤
  Relocate가 성공하고 후속 request는 target에서 처리된다.
- 세부 동작: [Host maintenance §6](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C9B Manual topology는 Relocate를 preflight에서 막는다

우선순위: `P0`

Framework가 replacement readiness를 증명할 수 없는 manual connection은 automatic handoff target으로 사용할
수 없다.

**검증 질문:** Manual-only topology의 Relocate가 `ManualTopologyUnsupported`로 끝나고 source를 유지하는가.

- 시작 조건: Manual RouteMesh 또는 ClientServer endpoint만 가진 fresh Host와 stateful source object가 있다.
- 절차: Public Relocate를 호출하고 terminal 뒤 source request를 보낸다. 이어서 explicit Shutdown을
  호출한다.
- 검증: Relocate는 blocked reason `ManualTopologyUnsupported`이며 source request가 성공한다. Shutdown은
  manual topology를 blocker로 사용하지 않고 bounded terminal로 끝난다.
- 세부 동작: [Host maintenance §6](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C10 Relocation mode가 정한 exact version만 선택한다

우선순위: `P0`

Weight가 높더라도 mode와 version filter를 통과하지 못한 target은 선택할 수 없다.

**검증 질문:** PlannedMaintenance는 N target, RollingUpdate N+1 요청은 N+1 target만 선택하는가.

- 시작 조건: Compatible N, N+1과 N+2 target이 모두 ready이며 제외 대상의 weight를 더 높게 설정한다.
- 절차: Fresh source에서 PlannedMaintenance와 RollingUpdate N+1을 각각 실행한다.
- 검증: 첫 result와 current locations는 N target, 두 번째는 N+1 target이다. N+2와 잘못된 version target의
  handler evidence는 없다.
- 세부 동작: [Host maintenance §5](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### OBS-C11 Concurrent Relocate option 충돌을 처리한다

우선순위: `P0`

같은 relocation intent의 caller는 진행 중인 operation 결과를 함께 기다릴 수 있지만 다른 intent가 그
operation의 target이나 deadline을 바꾸면 안 된다.

**검증 질문:** 같은 option caller는 같은 terminal을 받고 다른 option caller는
`OperationInProgress`인가.

- 시작 조건: RollingUpdate N+1 target restore를 application gate에서 보류할 수 있다.
- 절차: 첫 Relocate가 pending인 동안 같은 option, PlannedMaintenance와 RollingUpdate N+2 calls를
  시작한다. Target gate를 해제한다.
- 검증: 같은 option의 두 waiter는 동일한 terminal result를 받고 relocation은 한 번 수행된다. 다른 두
  calls는 `Blocked/OperationInProgress`이며 first operation의 effective version과 deadline을 바꾸지 않는다.
- 세부 동작: [Host maintenance §11](../spec/28-graceful-drain-handoff.ko.md)을
  검증한다.

#### OBS-C12 Relocate waiter cancellation과 Shutdown 경쟁을 구분한다

우선순위: `P0`

한 caller의 await cancellation은 shared Host operation을 취소하지 않는다. Concurrent Shutdown은 정식
경쟁 규칙에 따라 Relocate와 각각 terminal 결과를 가져야 한다.

**검증 질문:** Joined waiter만 취소되고 Relocate·Shutdown operation은 terminal 하나씩 반환하는가.

- 시작 조건: PlannedMaintenance target restore를 application gate에서 보류한다.
- 절차: 첫 Relocate와 같은 option의 두 번째 waiter를 시작하고 두 번째 waiter만 취소한다. Shutdown을
  시작한 뒤 target gate를 해제한다.
- 검증: 두 번째 caller만 cancellation이고 첫 Relocate waiter는 spec의 `ShutdownRequested` 경쟁 결과 또는
  이미 확정된 relocation result를 받는다. Shutdown은 `Stopped` 또는 `ForceStopped`로 한 번 끝나며 반복
  status 조회에서도 terminal 값이 바뀌지 않는다.
- 세부 동작: [Host maintenance §11](../spec/28-graceful-drain-handoff.ko.md)을
  검증한다.

## 5. 완료 기준

- Flow scenario는 정식 flow record field만, metric scenario는 public metric reader만 사용한다.
- Relocation과 Shutdown은 public Host status·result, object lookup, lifecycle callback과 client 결과로
  판정한다.
- Location row, relocation manifest, Core peer table, internal generation과 private progress counter는 E2E
  assertion이 아니다.
- Metric은 scenario 전후 delta로 비교하며 process 누적값이나 log flush 순서를 고정하지 않는다.
- Application gate와 public readiness로 순서를 제어하고 fixed settle sleep에 의존하지 않는다.
