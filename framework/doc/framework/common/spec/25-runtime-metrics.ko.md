---
title: "Runtime metric과 집계 규칙"
---

# Runtime metric과 집계 규칙

[스펙 목차](README.ko.md) · [이전: Runtime 상태 조회와 운영 진단](24-runtime-monitoring.ko.md) · [다음: Message flow tracing](26-message-flow-tracing.ko.md)

> **이 장이 정의하는 것** — 처리량·대기·실패·현재 개수를 집계하는 metric의 이름,
> 종류, 단위와 label.


## 1. 이 문서가 정의하는 계약

이 문서는 Framework의 처리량, 대기, 실패와 현재 개수를 집계하는 metric의 이름, 종류,
단위와 label을 정의한다. 모든 언어는 같은 계약으로 값을 기록하므로 하나의 dashboard와
alert rule을 공통으로 사용할 수 있다.

현재 runtime과 topology의 완전한 상태는
[Runtime 상태와 운영 진단](24-runtime-monitoring.ko.md), message 한 건의 진행 기록은
[Message flow tracing](26-message-flow-tracing.ko.md), host operation의 개별 결과는
[Host relocation와 shutdown](28-graceful-drain-handoff.ko.md)이 소유한다.

| 주체 | 책임 |
|---|---|
| Application | 표준 metric provider를 구성하고 수집한 값으로 dashboard와 alert를 만든다. |
| Framework | 이 문서의 이름·종류·단위·label로 값을 기록하며 message 처리 순서를 바꾸지 않는다. |
| Provider | 수집 주기, histogram bucket, aggregation, exporter와 backend를 정한다. |

Label 값의 종류는 application object나 message 수에 비례해 증가하지 않아야 한다.
Exporter, registry, 저장소, histogram bucket과 backend는 Framework public contract가
아니다.

## 2. 이름과 집계 규칙

Metric 표에서 `counter`는 발생 횟수나 누적량을 단조 증가시키고, `updown`은 현재 개수가
늘거나 줄 때 그 차이를 기록한다. `observable`은 provider가 수집하는 시점의 현재 값을
읽고, `histogram`은 operation마다 측정한 값을 분포로 기록한다.

- 계기 이름은 lowercase dotted ASCII인 `zlink.<surface>.<name>` 형식을 사용한다.
- 이름, label key와 허용 label value는 모든 언어에서 byte 단위로 같다.
- 시간 histogram의 단위는 초(`s`), byte 크기의 단위는 `By`, 나머지는 중괄호로 감싼
  count unit을 사용한다.
- Provider failure는 application callback, reply, 새 작업 수락과 host lifecycle 결과를
  바꾸지 않는다.

## 3. Host와 MeshNode

### 3.1 Host inbound dispatch

Framework가 수신했지만 handler가 아직 끝나지 않은 payload의 host 전체 byte 합계는
[Application HWM](01-glossary.ko.md#application-hwm)으로 제한한다. 다음 observable은 기존 dispatch
accounting 값을 읽으며 metric 수집을 위해 queue나 handler를 순회하지 않는다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.host.inbound.application_hwm` | observable | `By` | 없음 | Startup에서 적용한 host 전체 HWM이다. `0`은 제한하지 않는다는 뜻이다. |
| `zlink.host.inbound.pending_payload` | observable | `By` | `state` | 아직 terminal 상태가 아닌 application payload byte다. |
| `zlink.host.inbound.receive_paused` | observable | `{state}` | 없음 | HWM 때문에 application receive가 중단되었으면 `1`, 아니면 `0`이다. |
| `zlink.host.completion.pending_sends` | observable | `{request}` | 없음 | Reply 전송 permit을 기다리거나 확보한 request 수다. |
| `zlink.host.completion.send_limit` | observable | `{request}` | 없음 | Host completion send permit 상한이다. |

`zlink.host.inbound.pending_payload`의 `state`는 `queued|active`만 허용한다. MeshName, ChannelName,
Actor ID, Spot ID, packet name과 owner는 label로 사용하지 않는다. Owner별 top-N 진단은 metric이 아니라
명시적인 운영 조회로만 제공한다.

### 3.2 Peer와 channel

한 process에서 peer 연결과 Channel 메시징을 제공하는 runtime 단위를
[MeshNode](01-glossary.ko.md#meshnode)라고 한다. 여러 MeshNode가 같은 메시징 규칙을
공유하는 논리 runtime을 [RouteMesh](01-glossary.ko.md#routemesh)라고 한다. RouteMesh의
startup 등록 이름을 [MeshName](01-glossary.ko.md#meshname), Channel의 startup 등록 이름을
[ChannelName](01-glossary.ko.md#channelname)이라고 한다. Framework가 조건을 만족한
member 가운데 하나를 선택하는 동작을 [select-one](01-glossary.ko.md#select-one)이라고
한다.

기능별 serving 조건을 모두 만족한 [Ready 상태](01-glossary.ko.md#ready)의 peer와
member만 ready 계기에 포함한다. Remote MeshNode가 identity, endpoint, Channel 참여
정보와 상태를 알리려고 게시하는
[MeshNode descriptor](01-glossary.ko.md#meshnode-descriptor)가 configured peer 집계의
기준이다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.mesh_node.peers.configured` | observable | `{peer}` | `mesh_name`, `source` | 현재 descriptor에 존재하는 peer 수를 제공한다. |
| `zlink.mesh_node.peers.connected` | observable | `{peer}` | `mesh_name`, `source` | 현재 transport가 연결된 peer 수를 제공한다. |
| `zlink.mesh_node.peers.ready` | observable | `{peer}` | `mesh_name`, `source` | 새 작업 수락 조건과 handler readiness를 통과한 peer 수를 제공한다. |
| `zlink.mesh_node.channels.ready_members` | observable | `{member}` | `mesh_name`, `channel_name` | Select-one에 사용할 수 있는 member 수를 제공한다. |
| `zlink.mesh_node.channel.selection_failures` | counter | `{failure}` | `mesh_name`, `channel_name`, `reason` | Select-one에 사용할 member가 없어 operation을 시작하지 못한 횟수를 누적한다. |
| `zlink.mesh_node.requests.inflight` | updown | `{request}` | `mesh_name`, `surface` | 현재 reply를 기다리는 request 수를 제공한다. |
| `zlink.mesh_node.request.duration` | histogram | `s` | `mesh_name`, `surface`, `outcome` | Submit부터 terminal completion까지 걸린 request 시간을 기록한다. |
| `zlink.mesh_node.request.timeouts` | counter | `{request}` | `mesh_name`, `surface` | Request timeout 발생 횟수를 누적한다. |

| Label | 값 |
|---|---|
| `source` | `manual`, `redis`, `manual_and_redis` |
| Selection failure `reason` | `no_member`, `not_ready`, `draining` |
| `surface` | `node`, `channel`, `spot`, `instance_spot`, `actor` |

### 3.3 One-way message drop

Reply를 만들지 않고 송신 완료와 remote handler 완료를 분리하는 호출을
[one-way](01-glossary.ko.md#one-way-정상-완료)라고 한다. 이 절은 Framework가 remote
handler에 전달하지 못한 원인을 확정할 수 있을 때만 횟수를 기록한다.
`message_kind`는 handler namespace에서 send·request·publish 같은 호출 종류를 구분하는
[message kind](01-glossary.ko.md#message-kind)의 허용 값이다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.mesh_node.messages.dropped` | counter | `{message}` | `mesh_name`, `surface`, `message_kind`, `reason` | Framework가 원인을 확인한 one-way drop 횟수를 누적한다. |

Message drop `reason`은 `no_handler|decode_error|backpressure|stale_target|shutdown`이다.
이때 `backpressure`는 송신 경로나 queue의 capacity가 일시적으로 부족한
[상태](01-glossary.ko.md#backpressured)를 뜻한다.

Spot member 목록을 고정한 뒤 모든 대상에 보내는
[Logical Multicast](01-glossary.ko.md#logical-multicast)와 별도 PUB/SUB 연결로 event를
보내는 [classic fanout](01-glossary.ko.md#classic-fanout) publish는 제외한다. Target별
metric도 만들지 않는다.

## 4. Object와 STREAM

주소와 상태를 가지고 message를 받는 논리 실행 단위를
[Spot](01-glossary.ko.md#spot)이라고 한다. 이 절은 Spot과 그 안에서 application
message를 처리하는 Actor의 현재 개수와 capacity를 집계한다. Byte stream을
하나의 client와 server가 공유하는 연결 단위는
[STREAM session](01-glossary.ko.md#stream-session)이다.

Actor나 Spot을 다른 node에서 계속 실행할 때 application state를 다시 만들거나 저장해서
복원하는 방법을 [relocation policy](01-glossary.ko.md#relocation-policy)라고 한다.
Actor·Spot을 실제로 실행하고 application queue를 관리하는 MeshNode를
[owner](01-glossary.ko.md#owner)라고 한다. 현재 owner와 위치를 판단하는 기준 record를 보관하는
[Location Store](01-glossary.ko.md#location-store)의 확정값을 capacity 집계에 사용한다.
`zlink.spot.count`와 `zlink.actor.count`는 이 MeshNode가 지금 실행하고 있는 수를 세고,
`zlink.object.capacity.*`와 `zlink.spot.type.capacity.*`는 Location Store가 확정한
population을 읽는다. 두 계기는 집계 경계가 달라 서로를 대체하지 않으며 값이 다를 수 있다.
Spot 종류를 나타내는 [Spot kind](01-glossary.ko.md#spot-kind)와 startup 등록 뒤 바뀌지
않는 type identity인 [stable type](01-glossary.ko.md#stable-type)은 등록값으로만
label에 사용한다.
ID로 처음 호출할 때 Framework가 만들 수 있는 Spot을
[Instance Spot](01-glossary.ko.md#entry-spot-user-spot과-instance-spot)이라고 한다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.spot.count` | updown | `{spot}` | `mesh_name`, `spot_kind` | 현재 Spot 수를 제공한다. |
| `zlink.actor.count` | updown | `{actor}` | `mesh_name` | 현재 Actor 수를 제공한다. |
| `zlink.object.capacity.active` | observable | `{object}` | `mesh_name`, `capacity_scope` | Location Store가 확정한 active population 수를 제공한다. |
| `zlink.object.capacity.reserved` | observable | `{object}` | `mesh_name`, `capacity_scope` | Location Store reservation이 확보한 population 수를 제공한다. |
| `zlink.object.capacity.limit` | observable | `{object}` | `mesh_name`, `capacity_scope` | Actor 전체 또는 Spot 전체 limit을 제공하며, 값이 `0`이면 제한하지 않는다. |
| `zlink.spot.type.capacity.active` | observable | `{spot}` | `mesh_name`, `spot_kind`, `stable_type` | 등록한 Spot type의 active 수를 제공한다. |
| `zlink.spot.type.capacity.reserved` | observable | `{spot}` | `mesh_name`, `spot_kind`, `stable_type` | 등록한 Spot type의 reserved 수를 제공한다. |
| `zlink.spot.type.capacity.limit` | observable | `{spot}` | `mesh_name`, `spot_kind`, `stable_type` | 등록한 Spot type의 limit을 제공하며, 값이 `0`이면 별도로 제한하지 않는다. |
| `zlink.object.activation.active` | observable | `{activation}` | `mesh_name` | 현재 factory와 initialization을 실행 중인 수를 제공한다. |
| `zlink.object.activation.limit` | observable | `{activation}` | `mesh_name` | Population capacity와 별도로 적용하는 activation concurrency limit을 제공한다. |
| `zlink.relocation.started` | counter | `{relocation}` | `mesh_name`, `object_kind`, `policy` | Actor·Instance Spot relocation을 시작한 횟수를 누적한다. |
| `zlink.relocation.completed` | counter | `{relocation}` | `mesh_name`, `object_kind`, `policy`, `outcome` | Relocation terminal 결과를 누적한다. |
| `zlink.relocation.duration` | histogram | `s` | `mesh_name`, `object_kind`, `policy`, `outcome` | Prepare부터 terminal phase까지 걸린 시간을 기록한다. |
| `zlink.relocation.bytes` | histogram | `By` | `mesh_name`, `object_kind`, `policy` | 변경할 수 없는 relocation envelope의 크기를 기록한다. |
| `zlink.stream.connections.active` | updown | `{connection}` | `transport` | 현재 STREAM session 수를 제공한다. |
| `zlink.stream.connections.opened` | counter | `{connection}` | `transport` | STREAM session을 연 횟수를 누적한다. |
| `zlink.stream.connections.closed` | counter | `{connection}` | `transport`, `close_reason` | STREAM session을 닫은 횟수를 누적한다. |

| Label | 값과 제한 |
|---|---|
| `spot_kind` | 일반 Spot은 `entry|user|instance`, type capacity는 `user|instance`다. |
| `capacity_scope` | `actor|spot`. Entry Spot 내부 Actor는 `actor`에 포함한다. |
| `stable_type` | Startup에 등록하여 개수가 제한된 User·Instance Spot의 stable type만 사용한다. |
| `object_kind` | `actor|user_spot|instance_spot` |
| `policy` | `recreate|snapshot` |
| Relocation `outcome` | `completed|aborted|failed|shutdown` |
| `transport` | Startup 등록 시점에 정한 허용 값 중 하나다. |
| `close_reason` | `client_close|idle_timeout|heartbeat_timeout|server_shutdown|protocol_error|transport_error` |

Instance Spot은 다음 계기를 추가한다. `instance_spot_type`은 startup에 등록하여 개수가
제한된 type만 사용한다.
Spot 초기화와 최초 message 저장이 끝나기 전에 handler 실행을 막는
[activation barrier](01-glossary.ko.md#activation-barrier) 앞의 message 수와 byte 수도
집계한다.
Actor나 Spot의 현재 위치, owner와 generation을 판단하는 기준 정보를
[authority](01-glossary.ko.md#authority)라고 한다. Claim 충돌은 이 기준 정보와 요청의
Spot kind 또는 stable type이 일치하지 않을 때 기록한다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.instance_spot.activations` | counter | `{activation}` | `mesh_name`, `instance_spot_type`, `outcome` | Owner claim부터 Ready 또는 terminal 실패까지의 결과를 누적한다. |
| `zlink.instance_spot.activation.duration` | histogram | `s` | `mesh_name`, `instance_spot_type`, `outcome` | 첫 address resolve부터 message를 처리할 수 있는 Ready 상태 또는 terminal 실패까지 걸린 시간을 기록한다. |
| `zlink.instance_spot.pending.messages` | observable | `{message}` | `mesh_name`, `instance_spot_type` | Activation barrier 앞에서 기다리는 message 수를 제공한다. |
| `zlink.instance_spot.pending.bytes` | observable | `By` | `mesh_name`, `instance_spot_type` | 생성 결과가 정해질 때까지 최초 message를 보관하는 activation barrier 앞에서 예약한 payload byte 수를 제공한다. |
| `zlink.instance_spot.claim.conflicts` | counter | `{claim}` | `mesh_name`, `instance_spot_type`, `reason` | 현재 유효한 authority, Spot kind 또는 stable type이 요청과 충돌한 횟수를 누적한다. |
| `zlink.instance_spot.takeovers` | counter | `{takeover}` | `mesh_name`, `instance_spot_type`, `outcome` | 만료된 owner row를 caller claim이 교체한 결과를 누적한다. |

Activation `outcome`은 `ready|rejected|conflict|timed_out|shutdown|store_failure|fenced`,
claim `reason`은 `authority|spot_kind|spot_type|closing`, takeover `outcome`은
`claimed|lost|failed`만 허용한다.

## 5. Host relocation과 shutdown

Host가 새 작업 수락을 중단하고 이미 받은 작업과 resource를 정리하는 절차를
[drain](01-glossary.ko.md#drain과-draining)이라고 한다. Host `Shutdown`은 이 정리를
마치고 runtime과 infrastructure를 종료한다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.host.state` | observable | `{runtime}` | `state` | 현재 Framework runtime state 하나에 값 1을 기록한다. |
| `zlink.host.relocation.duration` | histogram | `s` | `mode`, `outcome` | Host `Relocate` 시작부터 `Relocated` 또는 `Blocked` result까지 걸린 시간을 기록한다. |
| `zlink.host.relocation.blocked` | counter | `{operation}` | `mode`, `reason` | `Blocked`로 끝난 host `Relocate` 수를 누적한다. |
| `zlink.relocation.interruption` | histogram | `s` | `unit_kind`, 선택형 `execution_mode` | Actor, Instance Spot 또는 User Spot 한 unit의 admission seal부터 target admission-open ACK까지 걸린 시간을 기록한다. `unit_kind`는 `actor`, `instance_spot`, `user_spot`이다. 1초 초과를 relocation failure로 바꾸지 않는다. |
| `zlink.host.shutdown.duration` | histogram | `s` | `outcome` | Host `Shutdown` 시작부터 terminal result까지 걸린 시간을 기록한다. |
| `zlink.host.shutdown.forced` | counter | `{operation}` | `reason` | 제한 시간 안에 정리를 끝내려고 남은 작업을 강제로 종료한 host `Shutdown` 수를 누적한다. |

`state`는 `preparing|serving|relocating|relocated|draining|stopped|error`다. Relocation
`outcome`은 `relocated|blocked`다. Shutdown `outcome`은 `stopped|force_stopped`다. Reason은
[Host relocation와 shutdown](28-graceful-drain-handoff.ko.md)의 식별자를 사용한다.

## 6. Location과 telemetry

Framework host가 현재 lifecycle의 등록 정보와 object ownership을 계속 사용할 권한은 정해진
시간마다 갱신하는 [owner lease](01-glossary.ko.md#owner-lease)로 증명한다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.location.store.errors` | counter | `{error}` | `operation` | Redis read·write·lease failure 횟수를 누적한다. |
| `zlink.location.owner_lease.renew.failures` | counter | `{failure}` | `scope_kind`, `scope_name` | Owner lease renew failure 횟수를 누적한다. |
| `zlink.location.owner_lease.renew.lateness` | histogram | `s` | `scope_kind`, `scope_name` | 예정 시각보다 owner lease renew가 늦어진 시간을 기록한다. |
| `zlink.observability.events.overflow` | counter | `{event}` | `source` | Runtime status와 trace를 전달하는 내부 telemetry queue overflow 횟수를 누적한다. |

`scope_kind`는 `mesh|channel`이다. `scope_name`에는 해당 MeshName이나 ChannelName을 쓴다.
`operation`은 `read|compare_exchange|relocation_put|relocation_get|relocation_delete|lease_renew|release`다.
Logical Multicast와 classic fanout publish는 집계하지 않는다.

## 7. Label cardinality

Label에는 startup 등록값이나 enum이 허용한 값만 사용한다.

Classic fanout subscriber가 받을 event 종류를 나누는 문자열을
[topic](01-glossary.ko.md#topic)이라고 한다. Spot의 전역 논리 주소인
[Spot ID](01-glossary.ko.md#spot-id)를 비롯한 개별 object·connection·operation identity는
label에 사용하지 않는다.

| 허용 | 금지 |
|---|---|
| `mesh_name`, `channel_name`, `scope_kind`, `scope_name`, 정적 `source`, `surface`, `message_kind`, `operation`, `outcome`, `reason`, `mode`, `object_kind`, `unit_kind`, `execution_mode`, `policy`, `spot_kind`, `capacity_scope`, 등록된 `stable_type`, 등록된 `instance_spot_type`, `transport`, `close_reason`, `state` | topic, Actor ID, Spot ID, RID, endpoint, session ID, relocation ID, user ID, correlation ID, flow ID, application metadata value, application state format·version |

`MeshName`, `ChannelName`과 `scope_name`은 host 등록값으로 닫혀 있을 때만 사용한다.
Payload에서 label을 만들지 않는다.
개별 Actor·Spot·message 흐름은 metric이 아니라
[Message flow tracing](26-message-flow-tracing.ko.md)에서 확인한다.

## 8. 수집 경계

각 언어는 표준 meter 또는 registry를 사용한다. Public API는 exporter, reader, storage와
histogram bucket을 구성하지 않는다.

- Metric을 끈 경로는 payload를 복사하거나 per-message label dictionary를 만들지 않는다.
- counter와 updown 갱신은 dispatch ordering을 바꾸지 않는다.
- Observable은 runtime이 이미 유지하는 제한된 크기의 집계값만 읽는다. Actor·Spot,
  mailbox 또는 Location Store record 전체를 순회해서 값을 만들지 않는다.
- Mailbox enqueue·dequeue와 turn마다 counter, timestamp 또는 histogram을 기록하지 않는다.
- Provider가 histogram bucket과 aggregation을 정한다.
- Provider callback failure는 마지막 정상 수집 결과를 소급해서 바꾸지 않는다.

## 9. 구현 및 contract test 검증 요구

- 계기 이름, 종류, 단위와 허용 label value가 모든 언어에서 같다.
- Mailbox·Spot·Actor queue와 turn 단위 metric이 존재하지 않으며 수집을 위해 전체 object나
  Store record를 순회하지 않는다.
- topic, Actor ID, Spot ID, RID, endpoint, correlation ID와 flow ID가 어떤 metric label에도 나타나지 않는다.
- Telemetry queue overflow와 provider failure가 dispatch와 host lifecycle 결과를 바꾸지 않는다.
- Host 계기와 label은 [Host relocation과 shutdown](28-graceful-drain-handoff.ko.md)의 result와 일치한다.
- Instance activation은 등록된 type 단위로 관찰하며 Spot ID·owner ID·generation은 label에서 제외한다.
- Instance one-way activation 실패는 `surface=instance_spot` drop이며 reply나 replay를 만들지 않는다.
- Public Framework interface에 exporter, reader, storage, bucket과 metric event DTO가 나타나지 않는다.
