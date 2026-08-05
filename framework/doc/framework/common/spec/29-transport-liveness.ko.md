---
title: "Transport 연결 상태 확인"
---

# Transport 연결 상태 확인

[스펙 목차](README.ko.md) · [이전: Host Relocate와 Shutdown](28-graceful-drain-handoff.ko.md) · [다음: 장애 대응과 failover 범위](31-failure-failover-policy.ko.md)

> **이 장이 정의하는 것** — remote service connection의 사용 가능 여부를 계속 확인하고
> 끊기면 다시 연결하는 방법.


## 1. Application에서 보이는 결과

이 문서는 Framework가 remote service connection을 사용할 수 있는지 계속 확인하고,
연결이 끊기면 다시 연결하는 방법을 정의한다.

Framework가 remote service connection의 사용 가능 여부를 계속 확인하는 동작을
service liveness 확인이라고 한다.

여러 runtime node가 message를 주고받는 연결 그룹을
[RouteMesh](01-glossary.ko.md#routemesh)라고 한다. 같은 Channel을 식별하는 등록
이름은 [ChannelName](01-glossary.ko.md#channelname)이다. Client가 같은
ChannelName의 Server 중 하나를 선택하는 연결은
[ClientServer Channel](01-glossary.ko.md#clientserver-channel)이다. 별도 PUB/SUB
socket으로 event를 전달하는 기능은
[Classic fanout](01-glossary.ko.md#classic-fanout)이다.

Framework는 세 연결 방식에 같은 시간 기준을 적용한다. 그러나 양방향 연결과
단방향 fanout은 연결 상태를 확인하는 방법이 다르다.

RouteMesh에서 양쪽 MeshNode의 Object role이 모두 `Client`이면 peer connection을
사용하지 않는다. Automatic discovery는 descriptor를 확인해 connection intent를
만들지 않는다. Manual connection은 handshake에서 두 role을 확인한 뒤 ready 전에
닫는다. Connection이 없으므로 이 pair에는 liveness probe와 deadline을 적용하지
않는다.

| 연결 방식 | Framework가 연결 상태를 확인하는 방법 |
|---|---|
| RouteMesh·ClientServer | 상대에게 확인 요청을 보내고 같은 ID가 든 응답을 기다린다. |
| Classic fanout | Publisher가 단방향 확인 record를 보내고 subscriber가 마지막 수신 시각을 확인한다. |

연결 상태 확인용 command, raw transport monitor와 timer는 application public API가 아니며,
Application message handler도 이 신호를 받지 않는다.

Store에 기록한 owner 사용 기한, request·reply와 push를 같은 연결에서 교환하는
[STREAM session](01-glossary.ko.md#stream-session)의 heartbeat, request timeout은
각각 목적이 다르다. Framework는 어느 하나를 다른 하나의 대체 신호로 사용하지
않는다. Runtime resource를 정리하는 [Shutdown](01-glossary.ko.md#shutdown)도
service liveness 실패와 별개의 operation이다.

## 2. 고정된 시간과 public API 경계

Framework는 마지막 정상 확인 뒤 connection을 유지할 수 있는 시간을 계산한다.
Operation을 끝내야 하는 마지막 시각을
[deadline](01-glossary.ko.md#deadline)이라고 한다.

| 설정 | 고정값 | 적용 범위 |
|---|---:|---|
| 연결 확인 주기 | 5초 | Framework service runtime의 모든 RouteMesh, ClientServer와 Classic fanout connection |
| Peer deadline | 15초 | Connection 하나가 정상 확인 없이 유지될 수 있는 시간 |

Framework builder는 이 두 값을 공개하지 않는다. Channel, handler 또는 peer마다
다른 값을 지정할 수도 없다.

각 언어의 service runtime은 binding의 public raw socket API와 Framework service
protocol만 사용한다. Private binding member, native symbol 직접 호출과 언어별
숨은 application option을 사용하지 않는다.

## 3. RouteMesh와 ClientServer

Transport 연결, service handshake와 identity 확인을 모두 통과하여 message target으로
사용할 수 있는 상태를 [ready](01-glossary.ko.md#ready)라고 한다. RouteMesh와
ClientServer는 ready가 된 시점부터 15초 deadline을 적용한다.

Manual RouteMesh에서 양쪽 모두 Object Client이고 RouteMesh Channel Server
membership도 없는 pair는 handshake admission에서 `NotRequired` terminal로 끝낸다.
이는 liveness failure나 reconnect 대기 상태가 아니다. Framework는 같은 endpoint와
configuration generation에서 connect를 반복하지 않는다. Endpoint, expected RID
또는 configuration generation이 바뀌면 새 intent로 다시 확인할 수 있다. Public
monitoring에는 이 peer를 `not_required`로 표시한다. 연결이 필요한데 ready
connection이 없는 `not_connected`와 구분하며, `not_required`는 probe·deadline과
liveness·health failure 집계에서 제외한다.

Framework는 application message가 없어도 5초마다 다음 순서로 연결을 확인한다.

1. 아직 응답받지 못한 ID가 없으면 connection 안에서 0이 아닌 새 ID를 만든다.
2. 해당 ID를 `livenessProbe`에 넣어 보낸다.
3. 응답을 기다리는 동안 다음 주기가 오면 새 ID를 만들지 않고 같은 ID를 다시
   보낸다.
4. Peer는 받은 ID를 `livenessAck`에 그대로 넣어 반환한다.
5. 현재 connection이 기다리는 ID와 같은 첫 ACK만 deadline을 다시 15초로 설정한다.

Connection 하나에는 아직 응답받지 못한 ID를 최대 하나만 유지한다.

| 받은 입력 | 현재 connection에 미치는 영향 |
|---|---|
| 기다리는 ID와 같은 첫 ACK | 해당 ID를 제거하고 deadline을 다시 15초로 설정한다. |
| 같은 ACK의 중복 수신 | 상태를 바꾸지 않는다. |
| 이전 probe ID의 ACK | 상태를 바꾸지 않는다. |
| 다른 physical connection의 ACK | 현재 connection의 증거로 사용하지 않는다. |
| 일반 application message | 진단용 마지막 수신 시각만 갱신하고 deadline은 연장하지 않는다. |

15초 안에 올바른 ACK를 받지 못하면 해당 connection을 not-ready로 바꾸고 닫는다.

Probe와 ACK는 Framework가 연결 상태만 확인하는 내부 신호다. 업무 payload나
metadata를 포함하지 않는다. Application queue에 넣거나 handler를 실행하지 않는다.

## 4. Classic fanout

Classic fanout의 PUB socket은 송신만 하고 SUB socket은 수신만 한다. Subscriber가
같은 physical connection으로 ACK를 보낼 수 없으므로 `livenessProbe`와
`livenessAck`을 사용하지 않는다. Subscriber는 publisher가 보내는 단방향 확인
record를 수신해 연결 상태를 판단한다.

Subscriber는 publisher connection을 서로 구분할 수 있도록 다음 규칙을 지킨다.

- Automatic discovery에서는 publisher descriptor마다 SUB socket과 receive loop를
  하나씩 만든다.
- Manual mode에서는 endpoint마다 SUB socket과 receive loop를 하나씩 만든다.
- 여러 publisher를 SUB socket 하나에 함께 연결하지 않는다.

이렇게 분리해야 한 publisher의 timeout이 다른 publisher를 not-ready로 바꾸지
않는다.

Publisher는 application event 전송 여부와 관계없이 5초마다 같은 PUB endpoint로
단방향 확인 record를 보낸다. 이 record를
[liveness beacon](01-glossary.ko.md#liveness와-liveness-beacon)이라고 한다.

| Frame | 정확한 값 |
|---|---|
| Topic frame | `01 5A 4C 46 31` |
| Payload frame | `5A 46 01 01` |
| Frame 수 | 정확히 2개 |

Application은 이 topic 전체와 같은 값을 fanout topic으로 사용할 수 없다. 지정하면
호출 인자 오류다. 같은 bytes로 시작하더라도 길이가 다르거나 나머지 bytes가 다르면
application topic으로 사용할 수 있다.

Subscriber는 publisher별 socket에서 다음 입력 중 하나를 처음 받은 뒤 해당
publisher를 ready로 표시한다.

- 형식이 올바른 application fanout record
- 형식이 정확한 liveness beacon

이후 둘 중 하나를 받을 때마다 마지막 수신 시각을 갱신한다. 15초 동안 아무것도
받지 못하면 해당 publisher만 not-ready로 바꾸고 전용 socket을 닫는다. 현재 연결
설정에 따라 새 socket으로 다시 연결한다.

Beacon은 application record와 같은 PUB socket을 사용하므로 Classic fanout의 손실
규칙을 함께 따른다. Subscriber의 수신 queue가 가득 찬 동안 발행된 beacon은
버려지고 나중에 다시 도착하지 않는다. 따라서 host가 15초 넘게 포화 상태를
유지하면서 fanout application traffic이 계속 queue를 채우면 해당 publisher는
not-ready가 된다. 이 판정은 오탐이 아니다. 그 시간 동안 subscriber는 application
record를 처리하지 못하는 상태다.

반면 한 peer가 수신 단계를 독점해서 다른 peer의 확인 신호가 밀리는 것은 오탐이다.
Framework는 한 connection에서 연속으로 수신하는 양에 상한을 두어, 한 peer의 전송량이
다른 peer의 ready 판정을 바꾸지 않게 한다. 상한에 도달하면 남은 수신을 다음 기회로
넘기고 다른 connection과 확인 신호 처리를 진행한다.

**이 상한은 Classic fanout에만 적용하는 것이 아니다.** 여러 connection을 한 수신 단계에서
처리하는 모든 경로 — RouteMesh, ClientServer, service connection, STREAM — 에 같은
규칙이 적용된다. 확인 신호는 어느 topology의 connection으로도 오며, 어느 경로에서든 한
connection이 수신 단계를 독점하면 같은 오탐이 생긴다.

상한은 **건수, byte, 경과 시간 셋을 함께 두고 먼저 닿는 것을 적용한다.** 한 축만 두면
다른 축으로 독점할 수 있다. 그리고 다음 회전은 **이번에 멈춘 connection의 다음부터**
시작한다. 항상 처음부터 순회하면 상한을 두어도 뒤쪽 connection이 계속 밀린다.

하나의 socket이 여러 peer를 대표하는 경우에는 socket이 아니라 **peer 단위로** 회계한다.
socket 단위로 세면 그 socket 뒤의 한 peer가 다른 peer의 몫까지 쓴다.

세 상한의 값은 아직 정해지지 않았다. 값이 정해지기 전까지 이 조항으로 판정할 수 있는
것은 "무한정 읽지 않는다"와 "회전 시작점이 이동한다"까지다.

Beacon은 application event가 아니다. Subscriber는 다음 동작을 하지 않는다.

- Publisher에 응답을 보내지 않는다.
- Application queue나 fanout handler에 전달하지 않는다.
- Application message trace를 만들지 않는다.
- Fanout application 수신 metric을 증가시키지 않는다.

Topic이 예약값인데 payload가 다르거나 frame 수가 2개가 아니면 protocol error다.
Subscriber는 해당 record를 application에 전달하거나 정상 수신으로 인정하지 않는다.
해당 publisher만 즉시 not-ready로 바꾸고 그 publisher의 socket만 닫는다.

## 5. Ready와 장애 판정

Remote endpoint와 identity를 찾도록 Store에 게시하는 정보를
[descriptor](01-glossary.ko.md#descriptor)라고 한다. Descriptor가 존재하거나 connect
요청이 수락됐다는 사실만으로 connection이 ready가 되지는 않는다.

| 연결 방식 | Ready가 되기 위한 조건 |
|---|---|
| RouteMesh·ClientServer | Transport 연결, service handshake, identity·generation 검증과 handler 준비를 모두 끝낸다. Server membership 없는 RouteMesh Object Client pair는 ready 대상에서 제외한다. |
| Classic fanout | Publisher별 SUB socket이 연결되고 descriptor 또는 manual endpoint 관계가 유효하며, 첫 정상 application record나 beacon을 받는다. |

다음 조건을 확인하면 해당 connection을 ready target 목록에서 즉시 제거한다.

- 상대가 orderly close를 보냈다.
- Transport 오류 또는 disconnect event를 받았다.
- RouteMesh·ClientServer peer deadline을 넘겼다.
- Fanout publisher가 15초 동안 아무 record도 보내지 않았다.
- Identity, 같은 RID를 사용한 서로 다른 process 실행을 구분하는
  [lifecycle generation](01-glossary.ko.md#lifecycle-generation) 또는 security
  확인에 실패했다.
- 현재 discovery descriptor와 같은 lifecycle generation을 가진 새 connection을
  승인해 기존 physical connection을 교체했다.
- Host가 `Draining`, `Stopped` 또는 `Error`가 되어 새 target 선택을 허용하지 않는다.

Orderly close와 transport disconnect는 15초를 기다리지 않는다. 이전 physical
connection에서 늦게 도착한 ACK나 frame은 새 connection의 상태를 바꾸지 못한다.

Peer 하나의 실패는 host 전체를 `Error`로 바꾸지 않는다. 다른 ready peer와 local
owner는 계속 처리한다. Ready peer가 없으면 Channel 호출은 `NotFound` 또는
`Unavailable`로 끝난다. Framework는 timeout을 늘려 실패를 숨기지
않는다.

## 6. Connection loss와 reconnect

Request와 reply를 같은 호출로 연결하는 식별 정보를
[reply correlation](01-glossary.ko.md#reply-correlation)이라고 한다. Framework는
이 값을 사용해 request의 최종 결과를 한 번만 완료한다.

| Connection을 잃은 시점 | Request 처리 |
|---|---|
| Transport가 request를 수락하기 전 | Route-not-connected로 끝낸다. |
| Transport 수락 여부를 알 수 없음 | 다른 peer에 자동 재제출하지 않는다. |
| 이미 수락됨 | Reply, request timeout, cancellation, `Shutdown` 또는 route failure 중 하나로 한 번만 끝낸다. |

Framework는 connection loss 뒤 request와 one-way message를 다른 peer나 owner에게
자동 제출하지 않는다.

Reconnect는 기존 configuration 또는 현재 discovery descriptor를 사용한다.

- RouteMesh와 ClientServer는 service handshake와 identity 확인을 다시 수행한다.
- Server membership 없는 RouteMesh Object Client pair의 `NotRequired` admission은 같은 manual configuration
  generation에서 reconnect하지 않는다.
- 이전 connection ID, reply route, session binding과 ready 상태를 재사용하지 않는다.
- Classic fanout은 해당 publisher용 SUB socket을 새로 만든다.
- Fanout connection은 첫 정상 record를 받기 전까지 ready가 아니다.
- 같은 RID라도 현재 discovery descriptor와 다른 lifecycle generation이면 새
  process 실행으로 처리한다. Generation 값의 숫자 크기는 비교하지 않는다.

## 7. Location Store와 host 종료

Host가 현재 owner 자격을 유지하는 기간을
[owner lease](01-glossary.ko.md#owner-lease)라고 한다. Owner lease와 descriptor는
discovery와 object 배치의 근거지만 transport connection이 ready임을 증명하지
않는다.

Store polling에 실패해도 이미 연결된 peer의 transport 상태 확인은 계속한다. 반대로
probe, ACK 또는 beacon을 받아도 만료된 owner lease나 object owner를 다시 유효하게
만들지 않는다.

Owner lease 갱신 주기와 transport 연결 확인 주기는 같은 값이 아니다. 두 값을 같은
public option으로 합치지 않는다.

Runtime이 stateful workload를 옮기는 `Relocate`나 `Shutdown`이 새 application
작업을 막은 뒤에도, 이미
수락한 reply·relocation·STREAM 처리에 필요한 connection은 deadline까지 유지할 수
있다. 이 connection을 새 target 선택에는 포함하지 않는다.

종료할 때는 connection을 닫기 전에 liveness timer, reconnect timer, transport
monitor subscription과 pending callback을 끝낸다.

## 8. 관측 정보

특정 시점의 runtime 상태를 읽기 전용으로 복사한 결과를
[snapshot](01-glossary.ko.md#snapshot)이라고 한다. Runtime snapshot은 다음 상태를
구분한다.

- Configured intent
- Connecting
- Admitted
- Ready
- Reconnecting
- Last failure

Orderly disconnect와 peer deadline 초과는 서로 다른 reason으로 기록한다. Metric
label에는 endpoint, RID와 connection ID를 넣지 않는다. 개별 identity는 항목 수가
제한된 snapshot과 trace에서만 제공한다.

## 9. 구현 및 contract test 검증 요구

| 범위 | 반드시 검증할 결과 |
|---|---|
| 양방향 확인 | RouteMesh·ClientServer가 application traffic 없이 5초마다 probe를 보낸다. Connection마다 대기 ID는 하나이며 ACK 전에는 같은 ID만 다시 보낸다. |
| ACK 판정 | 현재 connection의 현재 ID와 같은 첫 ACK만 deadline을 갱신한다. 중복·이전 ID·다른 connection의 ACK는 상태를 바꾸지 않는다. |
| Deadline | Half-open connection은 15초 안에 not-ready가 된다. Orderly close와 transport 오류는 즉시 반영한다. |
| 내부 신호 | Probe와 ACK는 application handler에 전달되지 않는다. 다른 inbound service frame은 peer deadline을 연장하지 않는다. |
| Fanout socket | Publisher마다 전용 SUB socket을 사용한다. 첫 정상 application record나 beacon 뒤에만 ready가 된다. |
| Fanout beacon | 정확한 topic·payload·2-frame 형식을 사용하고 ACK, application dispatch, trace와 application metric을 만들지 않는다. |
| 잘못된 beacon | 예약 topic의 형식 오류는 해당 publisher만 즉시 not-ready로 만들고 application에 전달하지 않는다. |
| 장애 격리 | Publisher 하나의 15초 timeout과 peer 하나의 실패가 다른 connection이나 host state를 `Error`로 바꾸지 않는다. |
| Store 분리 | Store polling 실패 중에도 transport 확인을 계속한다. Transport ready가 만료된 owner lease를 다시 유효하게 만들지 않는다. |
| Reconnect | Service handshake와 identity 확인을 다시 수행하고 이전 connection의 completion·binding·ready 상태를 재사용하지 않는다. |
| 불필요한 RouteMesh connection | Automatic은 양쪽 모두 Object Client이고 RouteMesh Channel Server membership도 없는 pair를 descriptor 단계에서 제외한다. Manual은 같은 조건의 pair를 ready 전에 `NotRequired`로 닫고 같은 configuration generation에 재시도하지 않으며 probe·deadline을 만들지 않는다. |
| 중복 방지 | Reply, timeout, cancellation, disconnect와 shutdown이 경쟁해도 request 결과를 한 번만 완료한다. 다른 peer나 owner에 자동 재제출하지 않는다. |
| 종료 정리 | `Relocate`·`Shutdown` 뒤 liveness·reconnect timer, subscription과 callback이 남지 않는다. |
| 언어 parity | C++·.NET·JVM·Node.js가 같은 고정 시간과 관찰 결과를 제공한다. |
