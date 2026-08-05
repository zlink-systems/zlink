<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: 관측·운영 배포](config-11-observability-ops.ko.md) | [다음: One-way submit admission](config-13-submit-admission.ko.md)
<!-- framework-adapter-nav:end -->

# Config 12 — ChannelName에 맞는 송신 경로 선택

Application은 `ChannelName`으로 message를 보낼 논리 대상을 지정한다. 한 process가 여러 RouteMesh와
ClientServer Channel을 함께 사용해도 Framework는 그 이름에 등록된 송신 경로 하나만 선택해야 한다.
같은 이름을 다른 송신 경로에 중복 등록하거나 준비되지 않은 경로를 다른 topology로 우회하면
Application이 의도하지 않은 server가 request를 처리할 수 있다.

이 config는 여러 process와 topology가 함께 있는 배포에서 Channel send·request, target 선택과 reply가
서로 섞이지 않는지 검증한다. Client는 역할 server의 application endpoint만 호출하며 private egress
index, descriptor record와 physical connection은 판정에 사용하지 않는다.

## 1. 확인 범위

- RouteMesh에서 양방향 Channel request와 remote Channel 선택
- Handler, Spot callback과 timer에서 시작하는 다른 Channel request
- ClientServer weight, drain, restart와 같은 process의 Client·Server 조합
- Channel role이 없거나 connection이 준비되지 않았을 때의 public 오류
- Process 안에서 같은 `ChannelName`을 여러 송신 경로에 등록한 startup 오류
- Listener의 port `0` bind와 advertised endpoint
- ClientServer one-way send

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Automatic RouteMesh와 ClientServer discovery가 같은 실행 namespace를 사용하게 한다. |
| Session server | 1 | `game` RouteMesh에서 `game.session` Server와 `game.play`, `game.api` 호출을 제공한다. |
| Play server | 1 | `game.play` Server이며 별도 `audit` RouteMesh의 `audit.record`와 ClientServer `workflow.command`를 호출한다. Spot callback과 timer도 제공한다. |
| API server | 2 | `game.api` Server membership을 구성하고 서로 다른 weight를 사용한다. |
| Workflow server | 2 | ClientServer `workflow.command` Server다. 한 variant는 같은 이름의 Client role도 함께 등록한다. |
| Workflow caller | 1 | ClientServer `workflow.command` Client다. |
| Audit server | 1 | `audit` RouteMesh의 `audit.record` Server다. |
| E2E client | 1 | 각 역할의 public application endpoint를 호출하고 reply와 application evidence를 수집한다. |

각 handler는 operation ID, 역할 이름, 받은 payload와 reply 값을 application state에 기록한다. Client는
이 값을 public evidence endpoint에서 조회한다. Listener endpoint와 Channel ready 상태는 정식 public
status API가 반환한 값만 사용한다.

## 3. 공통 실행과 판정 방법

Runner는 scenario마다 process, port와 Store namespace를 새로 만든다. 역할 server의 health와 해당
Channel의 public ready status를 확인한 뒤 message를 보낸다. Network 차단, process 종료와 재시작은
runner가 외부에서 수행한다.

Weighted selection은 적은 요청의 정확한 순서를 비교하지 않는다. 각 target이 100과 300 weight를 가질
때 800개의 서로 다른 request를 보내고 weight 300 target의 처리 비율이 65~85%이면 통과한다. 모든
request는 reply를 하나만 가져야 하며 handler count 합계도 800이어야 한다. 고정 sleep, exact alternation,
internal retry count와 log 순서는 판정에 사용하지 않는다.

## 4. Scenario

### Track A — ChannelName에 등록된 RouteMesh 경로를 사용

#### CH-E2E-01 같은 RouteMesh에서 양방향 request를 보낸다

우선순위: `P0`

두 process가 같은 RouteMesh에서 서로 다른 Server Channel을 제공하면 어느 쪽도 별도 reverse connection을
구성하지 않고 상대 Channel을 호출할 수 있어야 한다.

**검증 질문:** Session과 Play가 서로의 ChannelName으로 보낸 request를 상대 handler가 각각 한 번
처리하는가.

- 시작 조건: Session의 `game.play`와 Play의 `game.session`이 public status에서 ready다. 각 process의
  local Server membership은 자기 RouteMesh 후보가 아니므로, 호출자가 사용하는 remote Server 경로를
  확인한다.
- 절차: Session이 `game.play` request를, Play가 `game.session` request를 각각 한 번 보낸다.
- 검증: 각 request는 상대 역할의 marker가 포함된 reply를 하나만 받는다. 두 handler는 자기 request를
  각각 한 번 처리하며 다른 역할의 handler에는 같은 operation ID가 기록되지 않는다.
- 세부 동작: [Channel topology §4.2](../spec/07-channel-topology.ko.md)와
  [Channel messaging §3.2](../spec/08-channel-messaging.ko.md)를 검증한다.

#### CH-E2E-02 Handler가 다른 topology의 Channel을 호출한다

우선순위: `P0`

Play handler는 원래 request를 처리하는 중에 Audit RouteMesh와 Workflow ClientServer를 호출한다. 세
request의 reply가 섞이면 원래 caller가 다른 operation의 결과를 받을 수 있다.

**검증 질문:** Handler가 두 downstream Channel을 호출해도 각 reply가 원래 operation에 한 번만
연결되는가.

- 시작 조건: `game.play`, `audit.record`와 `workflow.command`가 모두 public status에서 ready다.
- 절차: Session이 Play에 operation ID가 포함된 request를 보낸다. Play handler는 같은 ID로 Audit과
  Workflow를 순서대로 요청하고 두 결과를 원래 reply에 넣는다.
- 검증: Audit과 Workflow handler는 해당 ID를 각각 한 번 처리한다. Session은 두 downstream 결과가
  포함된 reply 하나를 받고 별도 unsolicited message를 받지 않는다.
- 세부 동작: [ClientServer Channel §6.2](../spec/09-client-server-channel.ko.md)의
  nested request와 reply 구분을 검증한다.

#### CH-E2E-03 Spot callback과 timer에서 ClientServer request를 보낸다

우선순위: `P1`

Spot callback이 request를 기다리거나 timer가 같은 Spot 상태를 변경해도 Spot의 serial turn 계약은
유지되어야 한다. Downstream reply를 새 업무 packet처럼 dispatch하면 상태 변경 순서가 달라진다.

**검증 질문:** Spot callback과 timer가 ClientServer reply를 기다린 뒤 정해진 application state 순서로
완료되는가.

- 시작 조건: Play의 Spot, timer와 `workflow.command`가 ready다. Application state의 sequence는 `0`이다.
- 절차: Client가 Spot handler를 호출하여 Workflow request를 기다리게 한다. Handler 완료 뒤 public
  application endpoint로 timer를 예약하고 timer evidence가 나올 때까지 bounded polling한다.
- 검증: Application evidence는 `handler-start, workflow-reply, handler-end, timer-start,
  workflow-reply, timer-end` 순서이며 sequence는 단계마다 한 번 증가한다. Timer 결과를 고정 sleep으로
  추정하지 않는다.
- 세부 동작: [비동기 실행 정책 §2](../spec/05-async-execution-policy.ko.md)과
  [ClientServer Channel §6.2](../spec/09-client-server-channel.ko.md)를
  검증한다.

#### CH-E2E-06 같은 ChannelName을 여러 송신 경로에 등록하면 시작하지 못한다

우선순위: `P0`

한 process에서 같은 `ChannelName`이 RouteMesh와 ClientServer를 모두 가리키면 호출 시점에 어느 경로를
선택해야 하는지 결정할 수 없다. Framework는 listener를 공개하기 전에 구성을 거부해야 한다.

**검증 질문:** 중복 egress registration이 있는 host가 configuration error로 종료되는가.

- 시작 조건: Negative host가 `duplicate.channel`을 RouteMesh와 ClientServer Client에 public builder로
  각각 등록한다.
- 절차: Runner가 negative host를 시작하여 process terminal과 health를 확인한다. 이어서 서로 다른 이름을
  사용하는 정상 host를 시작한다.
- 검증: Negative host는 ready가 되지 않고 public configuration error로 종료된다. 정상 host는 두
  Channel이 ready가 되고 각각 한 번 호출할 수 있다.
- 세부 동작: [Channel topology §4.4](../spec/07-channel-topology.ko.md)를
  검증한다.

#### CH-E2E-07A 등록하지 않은 ChannelName은 NotFound다

우선순위: `P0`

Process에 송신 경로가 없는 이름을 호출할 때 다른 RouteMesh나 ClientServer를 대신 사용하면 잘못된
업무 handler가 실행된다.

**검증 질문:** 등록하지 않은 ChannelName의 request가 `NotFound`로 끝나는가.

- 시작 조건: Caller process에 `missing.channel`을 어떤 topology에도 등록하지 않는다.
- 절차: Caller endpoint가 `missing.channel` request를 한 번 시작한다.
- 검증: Public error kind는 `NotFound`이며 모든 역할 handler evidence에 operation ID가 없다.
- 세부 동작: [Channel messaging §3.3](../spec/08-channel-messaging.ko.md)을 검증한다.

#### CH-E2E-07B Local Server role만 있어도 remote member를 호출한다

우선순위: `P0`

RouteMesh Channel의 Server role은 handler를 제공하면서 같은 Channel membership에 request를 시작할 수
있다. 별도 Client role이 없다는 이유로 local handler를 직접 호출하거나 실패해서는 안 된다.

**검증 질문:** API server가 같은 ChannelName의 다른 ready API server를 정상 호출하는가.

- 시작 조건: 두 API server가 `game.api` Server로 ready이며 호출을 시작할 server에는 같은 이름의
  Client role을 추가하지 않는다.
- 절차: 첫 API server의 application endpoint가 `game.api` request를 20번 시작한다.
- 검증: 각 request는 reply 하나를 받고, 적어도 하나는 다른 process의 handler가 처리한다. Handler count
  합계는 20이며 operation ID 중복이 없다.
- 세부 동작: [Channel topology §4.2](../spec/07-channel-topology.ko.md)를
  검증한다.

#### CH-E2E-07C Known target에 연결할 수 없으면 Unavailable이다

우선순위: `P0`

Target membership은 알려졌지만 connection이 ready가 아니면 현재 사용할 수 없는 상태다. Framework는
다른 topology로 우회하거나 timeout까지 반복 제출하지 않는다.

**검증 질문:** Known target의 connection이 준비되지 않은 request가 `Unavailable`로 끝나는가.

- 시작 조건: Caller가 target descriptor를 발견한 뒤 runner가 해당 target으로 가는 network를 차단한다.
  Public status에서 Channel이 ready가 아님을 확인한다.
- 절차: Caller가 operation ID가 포함된 request를 한 번 보낸다.
- 검증: Request는 `Unavailable` terminal 하나로 끝나고 어느 handler에도 operation ID가 기록되지 않는다.
  다른 RouteMesh나 ClientServer가 처리하면 실패다.
- 세부 동작: [오류 모델 §4](../spec/32-framework-error-model.ko.md)와
  [§5](../spec/32-framework-error-model.ko.md)를 검증한다.

#### CH-E2E-11 ChannelName만으로 다른 MeshNode의 Server를 호출한다

우선순위: `P0`

Application은 target RID와 endpoint를 전달하지 않고 `ChannelName`으로 remote membership을 선택한다.
호출자에게 physical route 선택을 요구하면 Channel의 위치 투명성이 깨진다.

**검증 질문:** Session이 `game.api`라는 이름만 지정하여 remote API handler의 reply와 send 처리를
받는가.

- 시작 조건: Session과 두 API server가 같은 `game` RouteMesh에서 ready다.
- 절차: Session이 `game.api` request와 send를 각각 한 번 시작한다.
- 검증: Request는 API 역할 marker가 포함된 reply를 받고 send marker는 API handler 한 곳에 한 번
  기록된다. Caller endpoint는 MeshName, RID와 endpoint를 입력으로 받지 않는다.
- 세부 동작: [Channel messaging §3.2](../spec/08-channel-messaging.ko.md)의
  ChannelName select-one을 검증한다.

### Track B — ClientServer target을 선택하고 lifecycle을 처리

#### CH-E2E-04A ClientServer weight에 따라 target을 선택한다

우선순위: `P0`

여러 ClientServer Server가 ready이면 weight는 신규 request가 각 target을 선택하는 상대 비율을 정한다.
짧은 구간의 정확한 순서는 보장하지 않는다.

**검증 질문:** Weight `100:300`인 두 server가 충분한 request에서 대략 `1:3` 비율로 선택되는가.

- 시작 조건: 두 Workflow server가 각각 weight 100과 300으로 ready다.
- 절차: Workflow caller가 서로 다른 operation ID의 request 800개를 순차 또는 제한된 concurrency로 보낸다.
- 검증: 800개가 모두 reply 하나를 받고 handler count 합계가 800이다. Weight 300 server의 처리 비율은
  65~85%다.
- 세부 동작: [ClientServer Channel §5](../spec/09-client-server-channel.ko.md)를
  검증한다.

#### CH-E2E-04B Draining server는 신규 request에서 제외한다

우선순위: `P0`

Drain은 이미 받은 request를 마치게 하면서 신규 request 선택을 중단한다. 진행 중인 request까지 즉시
실패시키거나 새 request를 계속 받으면 무중단 종료가 불가능하다.

**검증 질문:** Drain 전에 수락한 request는 완료되고 drain 뒤의 신규 request는 다른 server가 처리하는가.

- 시작 조건: 두 Workflow server가 ready다. Server A handler는 application signal을 받을 때까지 첫
  request reply를 보류하도록 구성한다.
- 절차: 첫 request가 A handler에 도착한 것을 public evidence로 확인한다. A에 public drain operation을
  시작한 뒤 신규 request 50개를 보낸다. 마지막으로 A handler signal을 해제한다.
- 검증: 첫 request는 A의 reply로 한 번 완료된다. 신규 50개는 모두 B가 처리하며 A에는 추가 marker가
  없다.
- 세부 동작: [ClientServer Channel §7](../spec/09-client-server-channel.ko.md)을
  검증한다.

#### CH-E2E-04C Server 재시작 뒤 신규 request를 처리한다

우선순위: `P0`

Server process가 재시작되면 이전 lifecycle의 connection과 reply는 더 이상 current가 아니다. Client는
새 server가 ready가 된 뒤 시작한 request를 새 lifecycle에서 처리해야 한다.

**검증 질문:** Workflow server 재시작 뒤 application retry나 고정 settle 없이 첫 신규 request가
성공하는가.

- 시작 조건: Server A만 실행하고 정상 대조 request가 성공한 상태다.
- 절차: Runner가 A를 종료하고 public status에서 not-ready를 확인한다. 같은 역할을 새 process로
  시작하고 ready가 확인되는 즉시 새 operation ID의 request를 한 번 보낸다.
- 검증: 새 request는 재시작한 server의 lifecycle marker가 포함된 reply 하나를 받는다. 이전 process의
  marker가 새 request evidence에 나타나지 않는다.
- 세부 동작: [ClientServer Channel §8](../spec/09-client-server-channel.ko.md)을 검증한다.

#### CH-E2E-05 Client role이 없는 process는 ClientServer request를 시작하지 못한다

우선순위: `P1`

ClientServer에서는 Client role을 등록한 process만 server connection과 송신 경로를 가진다. Server role만
있는 process가 같은 이름을 호출해 local handler를 직접 실행해서는 안 된다.

**검증 질문:** Server role만 등록한 process의 ClientServer request가 `NotFound`로 끝나는가.

- 시작 조건: Negative Workflow process에는 `workflow.command` Server role만 등록한다. 별도 정상 caller는
  Client role로 ready다.
- 절차: Negative process와 정상 caller가 각각 request를 한 번 시작한다.
- 검증: Negative process의 request는 `NotFound`이며 handler가 실행되지 않는다. 정상 caller의 request는
  Workflow handler에서 한 번 처리된다.
- 세부 동작: [ClientServer Channel §3](../spec/09-client-server-channel.ko.md)의
  role 책임을 검증한다.

#### CH-E2E-10 ClientServer one-way send는 reply를 만들지 않는다

우선순위: `P0`

One-way send는 ready server 하나에 message를 제출하며 request reply를 기다리지 않는다.

**검증 질문:** ClientServer send가 handler 한 곳에서 한 번 처리되고 client reply를 만들지 않는가.

- 시작 조건: 두 Workflow server와 caller가 ready다.
- 절차: Caller가 고유 marker의 send를 한 번 제출하고 public handler evidence를 bounded polling한다.
- 검증: Send public terminal은 성공하고 server 한 곳의 handler에 marker가 한 번 기록된다. Client에는
  request completion이나 unsolicited payload가 없다.
- 세부 동작: [ClientServer Channel §6](../spec/09-client-server-channel.ko.md)을
  검증한다.

#### CH-E2E-12 같은 process의 Client와 Server도 일반 후보로 선택한다

우선순위: `P0`

한 process가 같은 ClientServer Channel의 Client와 Server를 역할별로 한 번씩 등록할 수 있다. Local
Server도 remote Server와 같은 weight 규칙을 적용하며 무조건 우선하거나 제외하지 않는다.

**검증 질문:** Local과 remote Server가 같은 weight이면 충분한 request에서 둘 다 선택되는가.

- 시작 조건: Workflow process A가 `workflow.command` Client와 Server를 함께 등록하고, process B가 같은
  이름의 Server를 등록한다. 두 server의 weight는 100이다.
- 절차: A의 application endpoint가 request 400개를 보낸다.
- 검증: 400개가 모두 reply 하나를 받고 local과 remote handler가 각각 35~65%를 처리한다. Handler count
  합계는 400이며 local 호출에도 remote와 같은 application result가 적용된다.
- 세부 동작: [ClientServer Channel §5.1](../spec/09-client-server-channel.ko.md)을
  검증한다.

### Track C — Channel handler가 다른 public target을 호출

#### CH-E2E-08 ClientServer handler가 Spot과 Actor를 연속 호출한다

우선순위: `P1`

ClientServer handler는 RouteMesh에 등록된 Spot과 Actor를 public state-address API로 호출할 수 있다. 각
operation의 identity와 reply가 섞이면 원래 ClientServer request가 잘못 완료된다.

**검증 질문:** Workflow handler가 Spot과 Actor request를 마친 뒤 두 결과로 원래 reply를 한 번
완료하는가.

- 시작 조건: Workflow server는 `game` RouteMesh의 Object Client이고, ready Spot과 Actor의 ID를
  application fixture로 가진다.
- 절차: Workflow caller가 operation ID가 포함된 request를 보낸다. Handler는 Spot request와 Actor
  request를 순서대로 실행한다.
- 검증: Spot과 Actor handler는 같은 operation ID를 각각 한 번 처리한다. Workflow caller는 두 결과가
  포함된 reply 하나를 받으며 별도 payload를 받지 않는다.
- 세부 동작: [ClientServer Channel §6.2](../spec/09-client-server-channel.ko.md)와
  [Spot messaging](../spec/12-spot-messaging.ko.md)을 검증한다.

### Track D — Remote listener 주소 확정

#### CH-E2E-09 Port 0과 advertised host로 remote connection을 만든다

우선순위: `P0`

Port `0`으로 bind하면 OS가 실제 port를 정한다. Wildcard bind 주소는 remote가 접속할 주소가 아니므로
Framework는 확정된 port와 `AdvertiseHost`를 조합하여 공개해야 한다.

**검증 질문:** 네 listener 종류가 port 0으로 시작해 public endpoint를 제공하고 remote client가 실제로
연결되는가.

- 시작 조건: RouteMesh, ClientServer, classic fanout publisher와 Stream server를 port 0, wildcard
  BindHost와 접근 가능한 AdvertiseHost로 구성한다.
- 절차: 각 listener의 public status가 ready가 된 뒤 실제 bound port와 advertised endpoint를 읽는다.
  대응하는 remote client가 각 topology의 정상 message를 한 번 보낸다.
- 검증: 모든 actual port는 0이 아니며 advertised endpoint에는 wildcard host와 port 0이 없다. 각 remote
  client가 대응 handler 또는 subscriber result를 받으며 다른 topology의 endpoint로 연결되지 않는다.
- 세부 동작: [Network listener identity §4](../spec/10-network-listener-identity.ko.md)와
  [§5](../spec/10-network-listener-identity.ko.md)를 검증한다.

## 5. 완료 기준

- 모든 절차와 판정은 public Framework API, public status와 역할 server의 application evidence만 사용한다.
- Internal egress index, descriptor record, reply token, socket 수와 physical connection ID는 통과 조건이
  아니다.
- Weighted scenario는 충분한 표본과 명시한 허용 범위를 사용하며 exact selection 순서를 요구하지 않는다.
- Readiness와 handler 완료는 bounded polling으로 확인하고 고정 sleep이나 log flush 순서에 의존하지 않는다.
- 모든 request는 reply, public error, timeout 또는 cancellation 중 terminal 결과 하나만 가져야 한다.
