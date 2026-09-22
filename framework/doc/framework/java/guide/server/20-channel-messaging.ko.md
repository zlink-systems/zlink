---
title: "Channel 메시징 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/20-channel-messaging.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Channel 메시징

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 3. 핵심 개념](03-concepts.ko.md) | [다음: Spot](21-spot.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/20-channel-messaging.ko.md) · [C#/.NET](../../../dotnet/guide/server/20-channel-messaging.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/20-channel-messaging.ko.md) · [Node/TypeScript](../../../node/guide/server/20-channel-messaging.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

이 장의 코드는 [tutorial README](https://github.com/zlink-systems/zlink-java-examples/blob/main/tutorial/README.ko.md)에서 가져왔다. [예제 저장소](https://github.com/zlink-systems/zlink-java-examples/blob/main/tutorial/README.ko.md)를 내려받아 README의 「내려받기와 설치」·「빌드」·「실행」 절을 따라 실행하면 아래 실행 결과를 재현할 수 있다.

!!! info "이 장을 읽고 나면"

    서버가 서로 호출하는 세 가지 구성을 등록하고 호출할 수 있다. 각 절의 코드는
    `framework/languages/dotnet/tutorial`에서 그대로 실행된다.

서버가 다른 서버를 호출할 때 상대의 주소를 지정하지 않는다. 호출하는 쪽은 **이름**만
지정하고, Framework가 그 이름을 맡은 **node**로 보낸다. node는 Framework를 올린 서버 process
하나를 가리킨다.

이 장은 그 이름이 **channel 이름**인 경우를 다룬다. 등록과 호출까지 다루며, 패턴별 제약과
선택의 경계는 [Channel 동작 원리](30-channel-patterns.ko.md)가 다룬다.

## 1. 요청과 응답

### 1.1 메시지 계약

계약은 평범한 record다. 별도 등록이나 attribute 선언이 필요하지 않다. 단방향 메시지에는
대응하는 응답 타입을 두지 않는다.

```java
--8<-- "framework/languages/java/tutorial/java/Shared/src/main/java/systems/zlink/tutorial/shared/Contracts.java:channel-contracts"
```

### 1.2 응답을 돌려주는 handler

handler의 반환값이 그대로 응답이 된다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/channel/GetPlayerProfileHandler.java:channel-request-handler"
```

### 1.3 응답이 없는 handler

`send`로 온 메시지를 받는다. 반환값이 없으므로 호출한 쪽은 처리 결과를 알 수 없다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/channel/RecordLoginHandler.java:channel-send-handler"
```

### 1.4 호출의 종결자 — 마지막 조각이 호출을 보낸다

이 장의 호출은 모두 **대상을 정하는 앞부분과 보내는 마지막 한 조각**으로 되어 있다. 대상 이름과
메시지를 고르는 앞부분은 호출을 준비할 뿐 아직 아무것도 보내지 않는다. 마지막 조각을 호출하는
순간에야 runtime이 그 호출을 받아들인다. **마지막 조각을 빠뜨리면 아무 일도 일어나지 않는다**
— 아래 탭의 호출 코드에서 줄 끝에 붙는 것이 그 조각이다.

종결자는 `request`와 `send`이며, **끝났다는 말의 뜻이 서로 다르다.**

`request`는 **상대의 응답이 도착했을 때** 끝난다. 실패하면 상대가 처리하지 못했거나 응답이
오지 않았다는 뜻이다. 받을 응답 타입은 보내는 메시지가 아니라 이 자리에서 정한다 — 같은 요청
payload로 서로 다른 응답 타입을 받는 자리가 있기 때문이다.

`send`는 **호출하는 쪽의 runtime이 전송을 접수했을 때** 끝난다. 성공은 보냈다는 뜻이지 처리됐다는 뜻이
아니다. 상대의 처리 실패는 알 수 없다. 처리 결과가 필요하면 `request`를 사용한다.

## 2. channel 메시징의 구성

Framework는 channel 메시징을 RouteMesh·ClientServer·Fanout 구성으로 제공한다. 구성마다 이름으로 호출하는 것은 같고, 다른 것은
**그 이름을 누가 받는가**다. 아래 절이 각각을 차례로 다룬다.

**[RouteMesh](#3-routemesh)** — 서버들이 서로를 호출하는 구성이다. 참여한 서버가 하나의 망으로 연결되어
어느 쪽이든 다른 쪽을 호출할 수 있다. 같은 이름을 맡은 서버가 여럿이면 Framework가 그중
하나에게 보낸다. 서버끼리 양방향으로 호출이 오가는 시스템이 여기에 해당한다.

**[ClientServer](#4-clientserver-channel)** — 웹 서버와 같은 구성이다. 서버가 포트를 열고 호출하는 쪽이 그 주소로
접속한다. 같은 일을 하는 서버를 여러 대 띄우면 호출하는 쪽이 그중 하나를 골라 보낸다. 호출은
한 방향이며, 서버가 호출한 쪽을 먼저 호출하지 않는다.

**[Fanout](#5-fanout-channel)** — 발행·구독 구성이다. 한 번 보내면 구독한 서버 전부가 받는다. 점검 공지처럼
모두가 같은 소식을 받아야 할 때 사용한다. 보내는 쪽은 누가 구독 중인지 알지 않는다.

세 구성 모두 [요청과 응답](#1-요청과-응답)의 메시지 계약과 handler를 그대로 사용한다. 바뀌는 것은 등록 방법이다.

!!! note "정해진 대상 하나가 받아야 하면 channel이 아니다"

    `player-123`의 상태를 바꾸는 메시지처럼 **받을 대상이 정해져 있으면** channel로 보내지
    않는다. channel은 그 이름을 맡은 서버 중 하나를 고르기 때문이다. 그런 메시지를 위한
    경로가 따로 있다 — [Spot·Actor 호출하기](#37-spotactor-호출하기).

세 구성의 차이를 항목별로 비교한 표와 제약은
[Channel 동작 원리](30-channel-patterns.ko.md)에 있다.

## 3. RouteMesh

RouteMesh는 **무엇을 지정하느냐**에 따라 호출이 달라진다. 지정할 수 있는 것은 다음과 같다.

- **channel 이름** — `requestToChannel` · `sendToChannel`. 그 이름을 맡은 node 중 하나가
  받는다 — [channel 이름으로 호출하기](#32-받는-쪽--channel을-담당하는-node).
- **node** — `requestToNode` · `sendToNode`. 지정한 그 node가 받는다 — [node를 직접 호출하기](#36-node를-직접-호출하기).
- **Spot id** — `requestToSpot` · `sendToSpot`. 그 id의 Spot이 받는다 —
  [Spot·Actor 호출하기](#37-spotactor-호출하기).
- **Actor id** — `requestToActor` · `sendToActor`. 그 id의 Actor가 받는다 —
  [Spot·Actor 호출하기](#37-spotactor-호출하기).

이 호출은 모두 같은 mesh 연결을 사용한다. **channel 등록은 첫 번째에만 필요하다.**

### 3.1 동작

<iframe class="zlink-diagram" src="/common/diagrams/20-routemesh-bidirectional.html" title="RouteMesh — 양쪽이 서로 호출한다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-routemesh-bidirectional.html" target="_blank">↗ 크게 보기</a></p>

같은 mesh에 참여한 node는 서로 호출할 수 있다. A가 `profile` channel을 담당하고 B가
`match` channel을 담당하면 A는 B를 호출하고 B는 A를 호출한다. 연결은 mesh 단위로 하나이며,
두 호출이 그 연결을 함께 사용한다.

channel 하나는 여전히 호출하는 쪽에서 담당 node로 가는 한 방향이다. 양방향은 **두 node가
각각 다른 channel의 담당으로 등록해서** 만든다.

같은 channel을 담당하는 node가 여럿이면 호출하는 쪽은 그중 하나를 지정하지 않는다.
Framework가 준비된 담당 node 중에서 하나를 고른다. node를 늘리면 후보가 늘어난다.

### 3.2 받는 쪽 — channel을 담당하는 node

!!! note "받는 쪽 process"

    이 절의 코드는 요청을 **처리하는** process에 들어간다.

mesh 이름은 양쪽 node가 동일하게 지정해야 한다. 그리고 `Server()`로 노출한 handler만 다른
node가 호출할 수 있다 — 같은 assembly에 있더라도 등록하지 않으면 호출 대상이 되지 않는다.

`listen`의 host는 현재 process가 socket을 bind할 주소이며 `0.0.0.0`은 모든 local interface에서
연결을 받는다. `advertiseHost`는
peer가 실제로 dial하고 Location Store의 MeshNode descriptor에 게시할 주소다. wildcard bind는 remote가
dial할 주소가 아니므로 단일 machine tutorial에는 `127.0.0.1`을 지정한다. 여러 host, container, NAT,
Kubernetes에서는 그 node에 도달 가능한 IP 또는 DNS를 지정하며, Pod IP 또는 pod별 DNS를 사용한다.
Service 하나로 여러 Pod를 나타내면 각 node endpoint를 구별할 수 없다. `advertiseHost`를 생략하면
  [Network listener identity §2.1](../../../common/spec/server/02-channel-transport/04-network-listener-identity.ko.md#21-기본값)의
규칙대로 non-wildcard bind host를 사용하고, wildcard `0.0.0.0`·`::`에는 같은 address family의 loopback
`127.0.0.1`·`::1`을 사용한다. advertised host에는 wildcard를 지정할 수 없다. 각 언어의 mesh 등록 code block은
그 언어의 option 표면을 그대로 보인다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:mesh-register"
```

### 3.3 호출하는 쪽 — 같은 channel을 client로 등록하는 node

!!! info "호출하는 쪽 process"

    이 절의 코드는 요청을 **보내는** 다른 process에 들어간다. 앞 절과 짝을 이룬다.

호출하는 node도 자신의 endpoint를 연다. 양쪽이 모두 열어야 peer로 연결된다. 연결은 mesh
단위로 하나이며, 여러 channel이 그 연결 하나를 이름으로 구분해 함께 사용한다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:channel-client-register"
```

### 3.4 호출

호출에는 channel 이름만 지정한다. 어느 node가 받을지는 지정하지 않는다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:channel-request-call"
```

### 3.5 실행 결과

tutorial README의 「실행」 절을 따라 띄운 상태에서 Server와 Client를 실행한 상태에서 Client의 HTTP 표면에 아래 `curl` 요청을 보내면, 첫 응답은 `curl` stdout에 나오고 login 기록은 Server process의 stdout 또는 `server.log`에 나온다.

```bash
curl http://127.0.0.1:5080/players/p1/profile
# {"playerId":"p1","nickname":"rookie","level":1}

curl -X POST http://127.0.0.1:5080/players/p1/logins
# 202. 받는 node 로그에 login recorded: p1
```

### 3.6 node를 직접 호출하기

여기까지는 어느 node가 받을지 지정하지 않았다. 두 번째 방식은 `RoutingId`로 MeshNode 하나를
지정한다. channel을 만들지 않고, 후보를 고르지도 않는다 — 지정한 node가 답하거나 호출이
실패한다.

!!! warning "운영 명령에만 사용한다"

    상태 점검이나 운영 명령처럼 **그 node 자체**가 대상일 때만 사용한다. Actor·Spot 생성 위치를
    고르거나 업무 메시지를 특정 서버에 고정하는 용도로 사용하지 않는다. 업무 메시지는 channel
    이름·Spot id·Actor id 같은 논리 이름을 사용한다 — Framework가 현재 담당을 고르므로 application은
    node RID를 보관하지 않는다.

<iframe class="zlink-diagram" src="/common/diagrams/20-node-direct.html" title="업무 호출은 이름, 운영 호출은 node RID" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-node-direct.html" target="_blank">↗ 크게 보기</a></p>

#### 받는 쪽 — mesh에 바로 등록한다

!!! note "받는 쪽 process"

    상태를 답하는 process에 들어간다.

`channel(...)`을 거치지 않는다. handler는 채널 요청 handler interface를 구현한다 —
정확한 이름은 아래 탭에서 그 언어의 것을 본다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ops/NodeStatusHandler.java:node-direct-handler"
```

받는 node는 **id를 고정해야 한다.** 고정하지 않으면 Framework가 생성한 id가 붙어 호출하는 쪽이
지정할 수 없다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:mesh-register"
```

#### 호출하는 쪽 — mesh에 연결되어 있으면 된다

!!! info "호출하는 쪽 process"

    운영 명령을 보내는 process에 들어간다.

**따로 등록할 것이 없다.** 그 node와 peer로 연결되어 있으면 id로 부를 수 있다. 아래 코드는
channel 호출에 사용한 것과 같은 등록이다.

연결할 때 기대하는 id를 함께 적을 수 있다. 연결을 그 node 하나로 묶는 것이며, 다른 id로 답하는
상대는 handshake에서 거부된다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:channel-client-register"
```

호출에는 mesh 이름과 대상 id를 함께 준다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:node-direct-call"
```

#### 실행 결과

tutorial README의 「실행」 절을 따라 띄운 상태에서 Server와 Client를 실행한 상태에서 Client의 HTTP 표면에 아래 `curl` 요청을 보내면, HTTP 응답은 `curl` stdout에 나오고 node-direct handler의 기록은 Server process의 stdout 또는 `server.log`에 나온다.

```bash
curl http://127.0.0.1:5080/ops/nodes/game-server-1/status
# {"meshName":"game","channelName":"(none)",
#  "calledBy":"game-2a0af167-...","uptime":"14s","processId":39892}

curl -i http://127.0.0.1:5080/ops/nodes/no-such-node/status
# 404. 후보를 고르지 않으므로 그대로 실패한다.
```

`channelName`이 비어 있다. channel이 관여하지 않았다는 뜻이며, channel handler였다면 이 자리에
channel 이름이 들어온다.

### 3.7 Spot·Actor 호출하기

**Spot**은 id로 찾는 상태 객체다. 채팅방 하나, 매칭 queue 하나처럼 무언가를 기억하면서 자기
앞으로 온 일을 한 줄로 세워 처리한다. **Actor**도 id로 찾는 실행 단위이며 플레이어 하나,
세션 하나처럼 개체 단위 상태를 맡는다.

둘은 mesh의 어느 node에서든 실행될 수 있고, 실행 중에 다른 node로 옮겨 갈 수도 있다. **그
대상이 지금 어느 node에 있는지는 RouteMesh가 찾는다.** 호출하는 쪽은 id만 준다.

<iframe class="zlink-diagram" src="/common/diagrams/20-spot-actor-routing.html" title="Spot·Actor는 id가 있는 곳으로 간다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-spot-actor-routing.html" target="_blank">↗ 크게 보기</a></p>

답을 기다리지 않는 호출은 id와 message만 준다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:spot-send-call"
```

답이 필요하면 같은 자리에 요청을 보낸다. 대상이 옮겨 가는 중일 수 있으므로 timeout을 함께
준다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:spot-request-call"
```

이것이 channel 호출과 다른 점이다. channel은 담당 node 중 **아무 하나**를 고르고, Spot·Actor는
**그 id의 대상이 있는 곳**으로 간다. 그래서 정해진 대상이 받아야 하는 메시지는 channel이
아니라 이 경로를 사용한다.

등록, lifecycle, 상태 관리, 위치 이동은 [Spot](21-spot.ko.md)과
[Actor](22-actor.ko.md)가 다룬다.

## 4. ClientServer channel

handler 작성 방법은 RouteMesh와 같다. 등록 방법만 다르다.

### 4.1 동작

<iframe class="zlink-diagram" src="/common/diagrams/20-clientserver-oneway.html" title="ClientServer — 호출은 한 방향이다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-clientserver-oneway.html" target="_blank">↗ 크게 보기</a></p>

server만 주소를 공개하고, 연결도 client가 시작한다. **server는 client를 대상으로 새 호출을
시작하지 않는다.** client에게 돌아가는 것은 client가 먼저 보낸 request의 응답뿐이다.

반대 방향 호출이 필요하면 반대쪽 node도 server로 세우고 별도 channel을 만든다. RouteMesh와
달리 등록 하나로 양방향이 되지 않는다.

### 4.2 받는 쪽 — 포트를 여는 서버

!!! note "받는 쪽 process"

    요청을 처리하는 process에 들어간다.

mesh와 별개로 자신의 포트를 열고 밖에서 접근할 주소를 따로 알린다. 호출하는 쪽이 이 주소로
직접 연결하기 때문이다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:clientserver-register"
```

### 4.3 호출하는 쪽 — 그 주소로 연결하는 client

!!! info "호출하는 쪽 process"

    요청을 보내는 process에 들어간다.

호출하는 쪽이 서버 endpoint를 직접 연결한다. 호출 코드 자체는 RouteMesh와 같다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:clientserver-client-register"
```

### 4.4 실행 결과

tutorial README의 「실행」 절을 따라 띄운 상태에서 ClientServer Server와 Client를 실행한 상태에서 Client의 HTTP 표면에 아래 `curl` 요청을 보내면, 응답은 `curl` stdout에 나오고 handler 기록은 Server process의 stdout 또는 `server.log`에 나온다.

```bash
curl -X POST http://127.0.0.1:5080/players/p1/tickets
# "ticket-p1"
```

## 5. Fanout channel

### 5.1 동작

<iframe class="zlink-diagram" src="/common/diagrams/20-fanout-topic.html" title="Fanout — 구독한 node 전부가 받는다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-fanout-topic.html" target="_blank">↗ 크게 보기</a></p>

발행하는 쪽은 받을 node를 지정하지 않는다. `publish` 한 번이 그 **topic**을 구독한 node
전부에 전달된다. topic을 생략하면 Framework가 event의 packet name을 topic으로 사용한다.

발행하는 쪽은 구독자 목록을 보관하지 않는다. 구독 node가 추가되거나 제거되어도 발행
코드는 변경되지 않는다.

!!! note "Fanout에는 응답이 없다"

    수신자가 여럿이므로 돌려받을 응답을 하나로 확정할 수 없다. Fanout client는 `publish`만
    제공하며 `request`에 해당하는 호출을 두지 않는다.

### 5.2 받는 쪽 — 구독하는 node

!!! note "받는 쪽 process"

    이벤트를 받는 process에 들어간다.

수신 handler는 RouteMesh·ClientServer와 다른 interface를 사용한다. 돌려줄 대상이 없으므로 반환값도 없다.

구독하는 쪽은 publisher 주소를 지정하지 않는다. Location Store가 등록되어 있으면 자동으로
해석하며, 수동 연결을 함께 지정하면 시작 단계에서 거부된다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/channel/MaintenanceNoticeSubscriber.java:fanout-handler"
```

### 5.3 호출하는 쪽 — 발행하는 node

!!! info "호출하는 쪽 process"

    이벤트를 보내는 process에 들어간다.

발행하는 쪽은 publisher로 등록한 뒤 publish를 호출한다. 받는 대상을 지정하지 않는다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:fanout-publish-register"
```

### 5.4 실행 결과

tutorial README의 「실행」 절을 따라 띄운 상태에서 fanout publisher와 subscriber가 포함된 Server·Client를 실행한 상태에서 Client의 HTTP 표면에 아래 `curl` 요청을 보내면, HTTP 응답은 `curl` stdout에 나오고 수신 기록은 subscriber process의 stdout 또는 `server.log`에 나온다.

```bash
curl -X POST http://127.0.0.1:5080/notices \
  -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
# 202. 구독한 node 로그에 maintenance notice: scheduled maintenance
```

## 6. 관련 문서

channel 이름 말고 다른 것을 지정해 호출하는 방법은 각 장이 다룬다.

- Spot id로 Spot 호출하기 — [Spot](21-spot.ko.md)
- Actor id로 Actor 호출하기 — [Actor](22-actor.ko.md)
- 외부 client 연결 — [STREAM](23-stream.ko.md)

더 깊이 볼 내용은 다음 문서가 다룬다.

- 배선, 대상 선택, 연결과 discovery, 시작 단계 검증 —
  [Channel 동작 원리](30-channel-patterns.ko.md)
- packet 이름, filter, codec — [Handler와 메시지 처리](31-handler-dispatch.ko.md)
- 이 장 코드의 실행본 — `framework/languages/dotnet/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
