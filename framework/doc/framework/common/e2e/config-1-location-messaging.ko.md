<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [다음: Spot 서비스](config-2-spot-service.ko.md)
<!-- framework-adapter-nav:end -->

# Config 1 — Location Store 기반 messaging

Application은 여러 server가 제공하는 같은 기능을 호출할 때 각 server의 endpoint를 직접 관리하지
않아야 한다. Provider가 시작되거나 종료되면 Framework가 Location Store에서 현재 위치를 확인하고,
새 request를 처리할 수 있는 provider만 선택해야 한다. 이 동작이 깨지면 server가 실행 중인데도
target을 찾지 못하거나, 종료된 process로 request를 계속 보내게 된다.

이 config는 provider와 consumer를 서로 다른 process로 실행하여 automatic discovery, peer 연결과
Channel messaging이 함께 동작하는지 검증한다. Client는 consumer의 업무 endpoint로 동작을 시작하고,
provider의 public application endpoint에서 handler 결과를 확인한다. Consumer와 provider는 public
Framework API만 사용한다. 따라서 E2E client가 Framework API, 내부 상태나 Location Store record를
직접 다루지 않는다.

## 1. 확인 범위

이 config는 다음 동작을 확인한다.

- Location Store에 등록된 현재 descriptor를 사용하여 peer endpoint를 찾고 ready connection을
  설정한다.
- Manual topology도 같은 identity 확인과 messaging 의미를 제공한다.
- Provider가 추가되거나 제거되면 이후 Channel target 선택에 현재 상태가 반영된다.
- Request, send, Node direct, timeout, handler 부재와 message 크기 제한이 정해진 결과로 끝난다.
- 서로 다른 Mesh에서 같은 global Actor·Spot ID를 만들려 해도 current object 하나로 수렴한다.
- 여러 process에서 public endpoint로 확인한 client 결과, public status와 application evidence가 서로
  일치한다.

## 2. 배포 구성

Runner는 아래 역할을 별도 process로 시작한다. Provider 추가, 종료와 crash를 검증하는 scenario만
process 수를 바꾼다.

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Redis Location Store | 1 | Provider의 MeshNode descriptor와 owner lease를 공유한다. 실행마다 다른 key prefix를 사용한다. |
| provider A·B | 2 | `profile` Channel의 request와 send를 처리한다. 각 process는 payload, RID와 handler 횟수를 evidence로 남긴다. 서로 다른 process에서 discovery와 target 선택을 검증한다. |
| consumer | 1 | HTTP로 받은 application 요청을 public Channel 또는 Node direct API로 실행한다. Public RouteMesh status도 HTTP 응답으로 제공한다. |
| Object Client pair | 2 | 두 Object Client 사이에 peer connection이 필요한 조건만 RM-A3에서 검증한다. 다른 scenario와 process를 공유하지 않는다. |
| Object owner | 2 | 서로 다른 Mesh에서 같은 stable Actor·User Spot type을 제공하고 public manager call과 direct message를 처리한다. |
| E2E client | 1 | 언어별 public HTTP client로 consumer의 업무 endpoint와 provider의 application evidence·control endpoint를 호출한다. Framework messaging API는 직접 호출하지 않는다. |

Provider와 consumer는 같은 `MeshName`인 `profile-mesh`를 사용한다. Provider A와 B는
`profile` Channel을 Server role로 등록하고 기본 weight `100`을 사용한다. Consumer는 같은 Channel을
Client role로 등록한다. Automatic topology에서는 각 process가 Location Store를 사용하고, Manual
topology에서는 runner가 provider endpoint를 consumer 구성에 넘긴다.

두 provider는 다음 fixture message를 처리한다.

- `ProfileLookupReq`를 받으면 요청 값, provider RID와 correlation marker를 담은
  `ProfileLookupRes`를 반환한다.
- `ProfileCommandMsg`를 받으면 command ID와 provider RID를 evidence에 한 번 기록한다.
- `NodePingReq`를 받으면 target RID와 source RID를 담은 `NodePingRes`를 반환한다.
- `ProfileLookupReq.Delay`가 설정되어 있으면 handler 완료를 늦춰 request timeout을 재현한다.

Message payload는 모든 언어가 별도 codec 등록 없이 Framework의 기본 typed JSON 경로로 처리한다.

## 3. 공통 실행과 판정 방법

Runner는 실행마다 port, RID prefix, Redis key prefix와 log 디렉토리를 새로 만든다. 이전 실행의
process, connection이나 Store record를 다음 실행에서 재사용하지 않는다.

Runner는 process의 port가 열렸다는 사실만으로 messaging 준비가 끝났다고 판단하지 않는다. Consumer가
공개 RouteMesh status에서 필요한 peer와 Channel target을 `ready`로 보고한 뒤에 client scenario를
시작한다. 상태 변경은 status stream이나 bounded evidence wait로 확인하며 고정 sleep으로 대신하지
않는다. 기본 대기 시간은 [E2E README §2.1](README.ko.md#21-로컬-e2e-대기-기준)을 따른다.

Client는 consumer의 업무 endpoint를 호출하여 message를 시작한다. 성공 여부는 다음 evidence를 함께
사용해 판정한다.

- Client 결과는 reply payload, public `ErrorKind`와 완료 시점을 확인한다.
- Consumer status는 ready peer, Channel ready target과 peer state를 확인한다.
- Provider evidence는 handler가 받은 payload, 처리 횟수와 provider RID를 확인한다.
- Structured log는 실패 원인을 조사할 때만 사용한다. 일반 messaging scenario의 통과 조건으로
  사용하지 않는다.

Provider evidence에는 scenario별 고유 correlation marker를 사용한다. 각 scenario가 끝나면 runner는
client 결과와 bounded counter를 수집하고 다음 scenario의 marker와 섞이지 않았는지 확인한다. 실패 시
모든 process의 log, client 결과와 evidence를 보존한다.

## 4. Scenario

### Track A — Peer를 찾고 ready connection을 설정

#### RM-A1 Location Store로 provider를 찾는다

우선순위: `P0`

Consumer는 provider endpoint를 알지 못한 채 시작한다. Provider가 Location Store에 현재 접속 정보를
등록하면 Framework는 그 정보를 읽고 transport identity를 확인한 뒤 connection을 ready 상태로 만든다.
이 과정이 없으면 application이 endpoint 목록을 직접 배포하고 갱신해야 한다.

**검증 질문:** Consumer가 endpoint를 지정하지 않아도 Location Store에서 두 provider를 찾아 첫
request의 reply를 받는가.

- 시작 조건: Provider A와 B가 서로 다른 process에서 시작되었고 같은 실행의 Location Store key
  prefix를 사용한다. Consumer 구성에는 provider endpoint가 없다.
- 절차: Runner는 consumer status에서 두 provider가 `ready`이고 `profile` Channel의 ready target 수가
  2가 될 때까지 기다린다. Client는 consumer의 profile 조회 endpoint를 한 번 호출한다.
- 검증: Client는 요청 marker와 선택된 provider RID가 들어 있는 `ProfileLookupRes`를 받는다. Provider
  A 또는 B 한 곳의 public application evidence에 같은 marker가 정확히 한 번 기록되고 다른
  provider에는 기록되지 않는다. Consumer의 public status에는 두 peer와 ready target 두 개가 있다.
- 세부 동작: [RouteMesh topology §6](../spec/07-channel-topology.ko.md)과
  [runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### RM-A2 Manual endpoint로 provider를 연결한다

우선순위: `P0`

고정된 배포에서는 Application이 peer endpoint를 직접 제공할 수 있다. 이 경우에도 Framework는
automatic topology와 같은 identity 확인을 수행하고, 양쪽이 동시에 연결하더라도 ready connection을
하나만 유지해야 한다.

**검증 질문:** Location Store 없이 Manual endpoint를 사용해도 public status에 ready peer 하나가
나타나고 같은 request 결과를 얻는가.

- 시작 조건: Object role이 `None`인 provider와 consumer를 fixed RID로 시작한다. 이 실행에는 Location
  Store를 등록하지 않는다.
- 절차: 첫 반복에서는 consumer에만 provider endpoint를 등록한다. 두 번째 반복에서는 양쪽에 상대
  endpoint를 등록하고 동시에 시작한다. 각 반복에서 ready peer가 하나가 된 뒤 profile 조회를 호출한다.
- 검증: 두 반복 모두 client가 같은 의미의 `ProfileLookupRes`를 받으며 provider handler는 한 번만
  실행된다. 양방향 반복에서도 consumer의 public status에는 해당 RID의 ready peer가 하나만 나타난다.
- 세부 동작: [RouteMesh topology §5.1](../spec/07-channel-topology.ko.md)과
  [§6](../spec/07-channel-topology.ko.md)의 Manual topology 계약을 검증한다.

#### RM-A3 Object Client pair의 connection 필요 여부

우선순위: `P0`

Object Client 두 개는 Actor나 Spot을 host하지 않으므로 object traffic을 받기 위한 peer connection이
필요하지 않다. 그러나 둘 중 하나가 RouteMesh Channel Server라면 그 node가 Channel message를 받아야
하므로 connection이 필요하다. Public status는 이 차이를 `not_required`와 `ready`로 구분해야 한다.

**검증 질문:** Object Client pair가 Channel Server membership 유무에 따라 `not_required`와 `ready`를
정확히 구분하는가.

- 시작 조건: 같은 MeshName을 사용하는 Object Client 두 process를 RM-A1과 분리하여 실행한다.
- 절차: 다음 구성을 각각 automatic과 manual topology에서 실행한다.
  1. 양쪽 모두 RouteMesh Channel membership을 등록하지 않는다.
  2. 한쪽 또는 양쪽에 Channel Client membership만 등록한다.
  3. 한쪽에 weight `100`인 Channel Server membership을 등록한다.
  4. 같은 Server membership의 weight를 `0`으로 설정한다.
- 검증: 첫 두 구성은 public peer state를 `not_required`로 표시하고 ready peer 수를 `0`으로 제공한다.
  뒤 두 구성은 peer state를 `ready`로 표시하고 ready peer 수를 `1`로 제공한다. Weight `0`은 새
  Channel target 선택에서는 제외되지만 Server membership과 connection 필요 여부를 없애지 않는다.
  `not_required`를 `not_connected` 또는 topology 장애로 표시하면 실패다.
- 세부 동작: [RouteMesh topology §4.2](../spec/07-channel-topology.ko.md),
  [§5.1](../spec/07-channel-topology.ko.md)과
  [runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### RM-A4 Provider replacement에 새 RID를 사용한다

우선순위: `P0`

Automatic topology의 RID는 process lifecycle을 구분한다. 같은 application 역할의 provider가 다시
시작되어도 이전 RID와 connection을 재사용하지 않아야 늦게 도착한 frame이나 상태가 새 process에
적용되지 않는다.

**검증 질문:** Provider를 정상 종료한 뒤 같은 역할로 다시 시작하면 consumer가 새 RID의 connection을
사용하는가.

- 시작 조건: Provider A v1과 consumer가 ready 상태이고 baseline request가 한 번 성공했다.
- 절차: Runner는 v1을 정상 종료하고 consumer status에서 v1이 ready peer와 Channel target에서 빠진
  것을 확인한다. 같은 RID prefix와 새 port를 사용하는 provider A v2를 시작한다. Consumer status에
  v2가 ready로 나타난 직후 application retry 없이 profile 조회를 호출한다.
- 검증: Public status에 나타난 v2 RID의 UUID suffix는 v1과 다르고 lowercase canonical UUID v4
  형식이다. 교체 뒤 request는 v2의 public application evidence에만 한 번 기록된다. Consumer를
  재시작하지 않으며 public status에 v1이 ready peer나 target으로 남으면 실패다.
- 세부 동작: [MeshNode §3.1](../spec/13-mesh-node.ko.md)과
  [transport liveness §6](../spec/29-transport-liveness.ko.md)을 검증한다.

#### RM-A6 서로 다른 RouteMesh를 격리한다

우선순위: `P1`

한 process와 Location Store가 여러 RouteMesh를 함께 사용해도 각 MeshName의 peer와 Channel target은
섞이지 않아야 한다. 그렇지 않으면 한 업무 영역의 scale 변경이 다른 영역의 routing을 바꿀 수 있다.

**검증 질문:** `profile-mesh`와 `workflow-mesh`가 같은 Store를 사용해도 각 request가 해당 Mesh의
provider에서만 처리되는가.

- 시작 조건: 두 MeshName의 provider와 두 Channel을 사용하는 consumer가 같은 Location Store key
  prefix에 등록되어 있다.
- 절차: Runner는 두 RouteMesh가 각각 ready가 될 때까지 기다린다. Client는 profile과 workflow 업무
  endpoint를 한 번씩 호출한다. 그 뒤 profile provider 하나만 정상 종료하고 두 endpoint를 다시 호출한다.
- 검증: 각 marker는 해당 Mesh의 provider evidence에만 기록된다. Profile provider 제거는
  `workflow-mesh` status, ready target 수와 handler evidence를 바꾸지 않는다.
- 세부 동작: [RouteMesh topology §3](../spec/07-channel-topology.ko.md)의 MeshName
  격리 계약을 검증한다.

#### RM-A7 Global Actor·Spot identity 충돌

우선순위: `P0`

Actor ID와 Spot ID는 Mesh별 주소가 아니라 Location Store namespace 전체에서 사용하는 global identity다.
서로 다른 Mesh에서 같은 ID를 동시에 만들더라도 Application에 object 두 개가 보이면 안 된다.

**검증 질문:** 서로 다른 Mesh의 concurrent GetOrCreate가 current Actor와 Spot 하나로 수렴하는가.

- 시작 조건: `profile-mesh`와 `workflow-mesh`의 Object owner가 같은 stable Actor·User Spot type을 제공하고 대상 ID는 Missing이다.
- 절차: 두 역할 server가 같은 Actor ID와 Spot ID로 public GetOrCreate를 동시에 호출한다. 각 terminal 뒤 manager `Find`와 global ID direct request를 실행한다.
- 검증: ID별로 current ref 하나가 반환되고 direct request는 그 ref의 owner에서만 한 번 처리된다. 다른 type·kind를 요청한 operation은 계약된 mismatch result로 끝나며 두 번째 object를 만들지 않는다.
- 세부 동작: [Location runtime](../spec/21-location-runtime.ko.md)과 [Spot 주소 messaging](../spec/16-spot-address-messaging.ko.md)을 검증한다.

### Track B — Provider 수와 lifecycle 변경

#### RM-B1 Traffic 처리 중 provider를 추가한다

우선순위: `P0`

Provider를 추가할 때 consumer를 재시작해야 한다면 automatic discovery의 운영상 이점이 사라진다.
Framework는 새 provider가 ready 상태가 된 뒤 이후 request의 선택 후보에 포함해야 한다.

**검증 질문:** Provider A가 request를 처리하는 동안 B를 추가하면 consumer 재시작 없이 B도 이후
request를 처리하는가.

- 시작 조건: Provider A만 ready target이며 baseline request 10개가 모두 A에서 처리되었다.
- 절차: Runner가 provider B를 시작한다. Consumer status에서 B가 ready peer이고 `profile` Channel의
  ready target 수가 2가 된 뒤 고유 marker를 가진 request 40개를 보낸다.
- 검증: 추가 전 marker는 A에만 있다. 추가 뒤에는 A와 B가 각각 한 건 이상 처리하고 두 provider의
  handler count 합이 40이다. Client request 40개는 모두 reply 하나로 끝난다.
- 세부 동작: [RouteMesh topology §7](../spec/07-channel-topology.ko.md)의
  ready target 선택을 검증한다.

#### RM-B2 Provider를 정상 종료한 뒤 target에서 제외한다

우선순위: `P0`

Provider가 정상 종료되면 Framework는 해당 provider를 신규 Channel target에서 제외해야 한다. 이
상태를 확인하지 않으면 consumer가 이미 종료된 endpoint로 계속 request를 보낼 수 있다.

**검증 질문:** Provider B의 정상 종료가 완료된 뒤 신규 request가 남아 있는 A에서만 처리되는가.

- 시작 조건: Provider A와 B가 ready target이고 baseline request에서 두 provider가 모두 처리했다.
- 절차: Runner가 B에 host shutdown을 요청한다. B의 terminal host status가 `stopped`이고 consumer
  status의 ready target 수가 1이 될 때까지 기다린다. 그 뒤 request 20개를 보낸다.
- 검증: 종료 뒤 보낸 20개는 모두 A evidence에 정확히 한 번 기록되고 client는 모두 정상 reply를
  받는다. B에는 종료 뒤 marker가 없으며 consumer의 public status에는 B가 ready peer나 target으로
  남지 않는다.
- 세부 동작: [MeshNode §8](../spec/13-mesh-node.ko.md)과
  [transport liveness §7](../spec/29-transport-liveness.ko.md)을 검증한다.

#### RM-B3 Provider crash 뒤 남은 provider를 사용한다

우선순위: `P0`

Provider 하나가 비정상 종료되어도 다른 ready provider는 신규 request를 계속 처리할 수 있어야 한다.
다만 crash 전에 A가 수락했을 수 있는 request를 B에서 자동으로 다시 실행하면 중복 side effect가 생길
수 있으므로 Framework는 그 operation을 B로 replay하지 않는다.

**검증 질문:** Provider A가 crash한 뒤 A가 target에서 제외되면 신규 request가 B에서 처리되고,
in-flight request는 B에서 자동 재실행되지 않는가.

- 시작 조건: Provider A만 ready target이며, 제어 가능한 request 하나가 A handler에서 시작되었다는
  evidence를 barrier로 사용한다.
- 절차: A handler를 완료시키지 않은 상태에서 runner가 provider B를 시작한다. Consumer public status에서
  A와 B가 모두 ready target이 된 것을 확인한 뒤 A process를 강제 종료한다. In-flight request의 terminal
  결과를 수집한다. Consumer status에서 A가 ready peer와 target에서 제외될 때까지 기다린 뒤 신규
  request 20개를 보낸다.
- 검증: In-flight request는 `Unavailable` 또는 설정한 deadline의 `DeadlineExceeded`로 한 번만 끝나며
  B evidence에는 같은 marker가 없다. A 제외 뒤 보낸 20개는 모두 B에서 한 번씩 처리되고 정상 reply를
  받는다. Consumer를 재시작하지 않으며 무한 대기나 자동 replay가 있으면 실패다.
- 세부 동작: [장애 대응 §3](../spec/31-failure-failover-policy.ko.md),
  [transport liveness §6](../spec/29-transport-liveness.ko.md)과
  [오류 모델 §5](../spec/32-framework-error-model.ko.md)를 검증한다.

### Track C — Ready connection에서 message를 처리

#### RM-C1 Request와 send의 완료 의미를 구분한다

우선순위: `P0`

Request는 reply를 받아야 완료되지만 send는 source outbound queue가 message를 수락하면 결과 없이
완료된다. Send 완료를 remote handler 완료로 해석하면 application이 아직 발생하지 않은 side effect를
확정한 것으로 잘못 판단할 수 있다.

**검증 질문:** Request는 typed reply로 끝나고 send는 결과 없이 완료되며, provider evidence가 두
message를 각각 한 번 기록하는가.

- 시작 조건: Provider A와 B가 ready target이고 scenario marker의 기존 evidence가 없다.
- 절차: Client가 consumer의 profile 조회 endpoint를 호출한 뒤 command endpoint를 호출한다. Send가
  완료된 뒤 provider의 bounded evidence wait로 command 처리를 별도로 기다린다.
- 검증: Request client는 marker와 provider RID가 담긴 reply를 받는다. Send client는 reply payload
  없이 정상 완료된다. Provider request와 command evidence에는 각 marker가 정확히 한 번 기록된다.
- 세부 동작: [오류 모델 §4](../spec/32-framework-error-model.ko.md)와
  [§5](../spec/32-framework-error-model.ko.md)를 검증한다.

#### RM-C2 RID를 지정한 Node direct request

우선순위: `P0`

운영 또는 infrastructure 호출은 Channel에서 provider를 자동 선택하는 대신 exact MeshName과 RID를
지정할 수 있다. Framework는 지정한 target을 다른 RID로 바꾸지 않아야 한다.

**검증 질문:** Node direct request가 지정한 provider에서만 처리되고 존재하지 않는 RID는
`NotFound`로 끝나는가.

- 시작 조건: Consumer status에서 provider A와 B의 RID를 확인했고 둘 다 ready다.
- 절차: Client가 consumer endpoint를 통해 B의 RID를 지정한 `NodePingReq`를 보낸다. 이어서 member
  snapshot에 없는 `api-missing` RID로 같은 request를 보낸다.
- 검증: 첫 request는 B evidence에만 한 번 기록되고 B RID가 담긴 reply를 받는다. 두 번째 request는
  `NotFound`로 한 번만 끝나며 어느 provider handler도 실행하지 않는다.
- 세부 동작: [Channel messaging §3.1](../spec/08-channel-messaging.ko.md)과
  [오류 모델 §5](../spec/32-framework-error-model.ko.md)를 검증한다.

#### RM-C3 같은 weight의 provider에 request를 분산한다

우선순위: `P0`

같은 Channel을 제공하는 ready provider가 둘이면 Framework는 둘 중 하나를 각 operation의 target으로
선택한다. 요청마다 정확히 번갈아 선택하는 순서는 공개 계약이 아니므로 전체 처리 횟수와 두 provider의
참여 여부로 판정한다.

**검증 질문:** 같은 weight의 provider 두 개에 request를 보내면 두 provider가 모두 처리하고 전체
handler 횟수가 request 수와 일치하는가.

- 시작 조건: A와 B가 weight `100`인 ready Channel target이다.
- 절차: Client가 서로 다른 marker를 가진 profile 조회 400개를 보낸다.
- 검증: 두 provider의 고유 marker 합은 400이고 중복 marker는 없다. 각 provider는 전체 request의
  35~65%인 140~260개를 처리한다. Client는 reply 400개를 받는다. 요청별 정확한 교대 순서는
  assertion으로 사용하지 않는다.
- 세부 동작: [Channel messaging §3.2](../spec/08-channel-messaging.ko.md)의
  select-one 계약을 검증한다.

#### RM-C4 Timeout 뒤 late reply를 버린다

우선순위: `P0`

Caller의 deadline이 먼저 끝나도 remote handler는 이미 실행 중일 수 있다. Framework는 늦게 도착한
reply를 다음 request의 결과로 사용하거나 끝난 request를 두 번째로 완료해서는 안 된다.

**검증 질문:** 느린 request가 `DeadlineExceeded`로 끝난 뒤 두 정상 request가 각각 자기 reply를
받는가.

- 시작 조건: Provider는 marker별 handler 시작과 완료를 evidence로 남기며 delay가 없는 baseline
  request가 성공했다.
- 절차: Client는 handler delay보다 짧은 deadline으로 느린 request를 보낸다. Timeout terminal을 받은
  직후 서로 다른 marker의 정상 request 두 개를 차례로 보낸다. Provider의 느린 handler 완료도 bounded
  evidence wait로 확인한다.
- 검증: 첫 request는 `DeadlineExceeded`로 한 번만 끝난다. 뒤의 두 request는 각 marker와 일치하는
  reply를 받는다. 늦은 reply가 client 결과를 추가하거나 다른 correlation marker에 연결되면 실패다.
- 세부 동작: [오류 모델 §5](../spec/32-framework-error-model.ko.md)의 timeout과
  late reply 계약을 검증한다.

#### RM-C5 Handler가 없는 message를 처리한다

우선순위: `P0`

Provider에 packet handler가 없으면 request caller는 대상 업무를 처리할 수 없다는 결과를 받아야 한다.
Send는 source queue가 수락한 뒤에는 이미 정상 완료될 수 있으므로, provider가 public message-flow
observer callback으로 받은 dispatch 결과를 application evidence에 기록한다.

**검증 질문:** 미등록 packet의 request는 `NotFound`로 끝나고 send는 application handler를 실행하지
않는가.

- 시작 조건: Provider에는 scenario에서 사용할 unknown packet identity의 handler가 없다.
- 절차: Client가 consumer endpoint를 통해 unknown request를 한 번 보내고, 다른 marker의 unknown send를
  한 번 보낸다. 이어서 정상 profile request를 보낸다.
- 검증: Unknown request는 `NotFound`로 한 번만 끝나고 observer evidence에는 `no_handler`와
  `reply_error`가 기록된다. Unknown send는 reply payload 없이 끝나며 observer evidence에는
  `no_handler`와 `drop`이 기록된다. 두 message 모두 application handler 실행은 `0`건이다. 정상
  request는 영향 없이 reply를 받는다.
- 세부 동작: [Channel messaging §5](../spec/08-channel-messaging.ko.md),
  [오류 모델 §4](../spec/32-framework-error-model.ko.md)와
  [message-flow tracing §3.1](../spec/26-message-flow-tracing.ko.md)을 검증한다.

#### RM-C7 Weight에 따라 provider 선택 비율을 정한다

우선순위: `P1`

Application은 같은 Channel을 제공하는 provider의 선택 비율을 startup weight로 정할 수 있다. 이
scenario는 두 provider를 처음부터 다른 weight로 시작하여 runtime 전파 시점을 별도로 기다리지 않고
장기 선택 비율을 검증한다.

**검증 질문:** A의 weight를 `300`, B의 weight를 `100`으로 설정하면 충분한 request에서 A가 전체의
약 75%를 처리하는가.

- 시작 조건: A는 startup weight `300`, B는 startup weight `100`으로 시작하며 둘 다 ready target이다.
- 절차: Client가 서로 다른 marker를 가진 profile 조회 800개를 보낸다.
- 검증: 두 provider의 고유 marker 합은 800이고 중복 marker는 없다. A는 전체 request의 65~85%인
  520~680개를 처리한다. Client는 reply 800개를 받는다. 정확한 요청별 순서나 정확히 3:1인 결과는
  요구하지 않는다.
- 세부 동작: [Channel messaging §3.2](../spec/08-channel-messaging.ko.md)의
  positive weight 장기 선택 비율을 검증한다. 실행 중 weight 제외·복원은
  [Config 5 RL-B4](config-5-resilience-lifecycle.ko.md)가 검증한다.

#### RM-C8 RouteMesh SS payload 무결성을 검증한다

우선순위: `P1`

RouteMesh ServerServer(SS)는 Framework-level `MaxMessageSize` 설정을 제공하지 않는다. 이
scenario는 SS transport에 listener 상한을 추가하지 않고, 여러 크기의 정상 payload가 손상 없이
왕복하는지 확인한다. StreamNode의 Core STREAM inbound 상한은 별도 계약이며 이 scenario의 대상이
아니다.

**검증 질문:** RouteMesh SS가 별도의 Framework message-size 설정 없이 여러 크기의 payload를
손상 없이 처리하는가.

- 시작 조건: Provider와 Consumer는 RouteMesh SS의 public topology만 설정하고 Framework
  `MaxMessageSize`를 설정하지 않는다. Provider는 payload 길이와 checksum을 reply와 evidence에
  기록한다.
- 절차: Client가 1 byte, 4 KiB, 256 KiB, 1 MiB payload를 각각 request로 보낸 뒤 1 byte 정상
  request를 한 번 더 보낸다.
- 검증: 각 payload request는 입력과 같은 길이와 checksum의 reply를 받고 provider handler가 한 번
  실행된다. 마지막 정상 request도 reply를 받으며, payload 일부만 전달된 evidence가 없다.
- 세부 동작: [RouteMesh topology §8](../spec/07-channel-topology.ko.md)의 SS 경계를 확인한다.
  StreamNode 상한은 [STREAM session §4](../spec/19-stream-session.ko.md#4-stream-socket-message-size)에서
  별도로 정의한다.

#### RM-C9 Application HWM 도달 뒤 수신을 재개한다

우선순위: `P2`

Provider의 handler가 처리 중인 payload가 public Application HWM에 도달하면 Framework는 새 application
message 수신을 멈춘다. Handler가 완료되어 pending payload가 HWM보다 작아지면 같은 connection에서
수신을 다시 시작해야 한다.

**검증 질문:** Provider의 pending payload가 Application HWM에 도달하면 public status가 수신 중단을
표시하고, handler 완료 뒤 수신 재개와 정상 request 처리를 표시하는가.

- 시작 조건: Provider의 `ApplicationHwmBytes`는 1 MiB다. Provider handler는
  public application control endpoint가 release할 때까지 2 MiB command 처리를 완료하지 않는다.
  Baseline request는 성공한 상태다.
- 절차: Client가 consumer를 통해 2 MiB command를 한 번 보낸다. Provider의 public application
  evidence에서 handler 시작을 확인한 뒤 public host status를 읽는다. Client가 provider의 public control
  endpoint로 handler를 release하고, public host status에서 pending payload 감소와 수신 재개를 확인한
  뒤 정상 profile request를 보낸다.
- 검증: Handler가 대기하는 동안 public status의 pending payload는 HWM 이상이고 application receive
  paused 값은 `true`다. Handler 완료 뒤 pending payload는 `0`, application receive paused 값은
  `false`가 된다. 이후 정상 request는 reply를 받고 provider handler가 한 번 실행되며 public RouteMesh
  status는 ready 상태를 유지한다. Outbound queue 길이나 send timeout 발생 횟수는 판정하지 않는다.
- 세부 동작: [Framework API §2.1](../spec/06-framework-api.ko.md),
  [runtime monitoring §2.1](../spec/24-runtime-monitoring.ko.md)과
  [오류 모델 §4](../spec/32-framework-error-model.ko.md)를 검증한다.

## 5. 완료 조건

- `P0` scenario인 RM-A1, RM-A2, RM-A3, RM-A4, RM-A7, RM-B1, RM-B2, RM-B3, RM-C1, RM-C2,
  RM-C3, RM-C4와 RM-C5가 모두 통과한다.
- 각 scenario는 client 결과와 역할 server evidence를 함께 사용한다. Public status가 필요한 판정은
  해당 runtime을 소유한 consumer 또는 provider 역할 server가 읽은 immutable snapshot이나 status
  stream을 사용한다.
- Client는 역할 server의 public 업무·evidence·control endpoint만 호출한다. Framework API, Store
  provider와 private record를 직접 호출하지 않는다.
- 모든 대기는 public readiness, public status sequence, 역할 server의 public application marker 또는
  bounded evidence wait를 기준으로 끝낸다. Scenario 순서를 맞추기 위한 고정 sleep을 사용하지 않는다.
- Redis key와 evidence marker는 실행별로 격리하고, 실행 종료 뒤 전용 key를 정리하거나 disposable
  Redis instance를 폐기한다.
- 실패 시 client 결과, consumer public status와 provider application evidence를 보존한다. Structured
  log는 어느 process와 단계에서 실패했는지 조사하는 진단 자료로만 보존한다.
