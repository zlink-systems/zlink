---
title: "ZLink Framework 개요"
---

# ZLink Framework 개요

[스펙 목차](README.ko.md) · [이전: Framework 메시징 용어집](01-glossary.ko.md) · [다음: ZLink Framework 상호작용 모델](03-interaction-model.ko.md)

> **이 장이 정의하는 것** — Framework가 언어별 service runtime에게 공유시키는 것(공개
> 계약·wire schema·fixture)과 각 언어가 독립적으로 구현하는 것의 경계.


## 1. 한 줄 정의

ZLink Framework는 typed message handler, RouteMesh, ClientServer Channel, Spot, Actor, STREAM
session, classic fanout과 location runtime을 application host의 lifecycle과 DI에 연결하는 상위 계층이다.

C++·.NET·JVM·Node.js Framework는 service runtime을 각 언어에서 구현한다. 이 runtime은 설치된 해당 언어
binding의 public raw socket API만 사용한다. 언어가 함께 사용하는 것은 public contract, versioned protocol
schema와 검증 fixture이며 공통 native runtime이나 service C ABI는 사용하지 않는다. Java와 Kotlin은 하나의
JVM runtime을 공유한다.

## 2. RouteMesh와 MeshNode

`RouteMesh`는 같은 `MeshName`을 공유하는 MeshNode의 물리 연결 범위다. [MeshNode](01-glossary.ko.md#meshnode) 하나는 routing ID 하나와
ROUTER endpoint 하나를 가진다. Channel handler를 제공하는 MeshNode는 하나 이상의 immutable
`ChannelName`에 Server로 참여하고, 호출 전용 또는 Node direct 전용 MeshNode는 ChannelName membership 없이
동작할 수 있다.

`MeshName`과 `ChannelName`은 역할이 다르다.

| 이름 | 의미 |
|---|---|
| MeshName | 서로 메시징할 수 있는 물리 mesh와 RID namespace |
| [ChannelName](01-glossary.ko.md#channelname) | [RouteMesh](01-glossary.ko.md#routemesh) 또는 ClientServer 송신 경로를 고르는 process-local 논리 주소 |

한 process는 서로 다른 [MeshName](01-glossary.ko.md#meshname)의 MeshNode를 여러 개 가질 수 있다. 각 mesh는 독립적이며 자동 relay를
제공하지 않는다. RouteMesh ChannelName을 추가해도 socket이나 endpoint가 추가되지 않는다. 같은 process의
ChannelName 하나는 RouteMesh 또는 ClientServer topology 하나에만 대응하고, 호출 역할에서는 그
topology의 송신 경로 하나를 가리킨다.

## 3. 메시지 대상

MeshNode 위의 메시징은 대상 선택 방식으로 구분한다.

- [node direct](01-glossary.ko.md#node-direct)는 같은 MeshName의 RID 하나를 지정한다.
- channel send/request는 ChannelName으로 process-local 송신 경로를 찾고, 그 RouteMesh member 또는
  ClientServer server 가운데 ready target 하나를 선택한다.
- [Spot](01-glossary.ko.md#spot) Logical Multicast는 ChannelName의 remote MeshNode와 node-local Spot subscription을 대상으로 한다.
- Spot과 Actor message는 global Spot ID 또는 Actor ID를 사용한다. Framework는 current Ready authority를
  resolve하고 owner mailbox로 전달한다.
- Spot·Actor create와 get-or-create는 manager의 명시적인 operation이다. Framework는 object role, capacity,
  stable type, capacity와 node-wide weight로 target을 선택하고 [Ready](01-glossary.ko.md#ready) barrier 뒤 immutable ref를 반환한다.

선택과 submit은 하나의 operation이다. application은 peer 목록이나 선택된 RID를 받아 별도 send를
반복하지 않는다.

## 4. Logical Multicast와 classic fanout

Spot [Logical Multicast](01-glossary.ko.md#logical-multicast)는 room, stage, zone처럼 위치가 바뀔 수 있는 logical Spot에 event를 전달한다.
송신 MeshNode는 target channel의 remote MeshNode마다 routed message를 한 번 보내고, 수신 MeshNode가
자기 node의 [subscription](01-glossary.ko.md#subscription)을 검사한다. 같은 node에서 여러 Spot이 일치하면 immutable message storage의
reference를 공유해 각 Spot queue에 넣는다.

Logical Multicast는 각 remote MeshNode에 내부 ROUTER 송신을 한 번씩 제출하고, local Spot queue에도
독립적으로 제출한다. remote 송신의 HWM, send timeout과 backpressure는 ROUTER 규칙을 그대로 따르며,
뒤 target의 실패가 앞에서 수락된 target의 제출을 취소하지 않는다.

[classic fanout](01-glossary.ko.md#classic-fanout)은 연결되어 있고 subscription 준비가 끝난 subscriber에게 event를 보내는 독립 PUB/SUB
기능이다. Automatic discovery를 사용하는 publisher는 전용 location descriptor에 실제 endpoint를 게시하고,
automatic subscriber는 같은 ChannelName의 live publisher를 모두 연결한다. Manual endpoint만 사용하는
publisher와 subscriber는 location store 없이 구성할 수 있다. MeshNode나 Spot이 필요하지 않은 host도 사용할
수 있으며 저장과 replay를 보장하지 않는다.

## 5. 실행 owner

Framework는 메시지를 실제 상태를 소유하는 실행 단위로 전달한다.

| [owner](01-glossary.ko.md#owner) | 책임 |
|---|---|
| Node | RID direct와 ChannelName handler, node에서 시작한 completion |
| Spot | Spot direct, Logical Multicast subscription, timer와 Spot 상태. Instance Spot은 direct와 timer만 사용하고 Actor [membership](01-glossary.ko.md#membership)과 Logical Multicast subscription은 사용하지 않음 |
| Actor | Actor direct message, Actor lifecycle과 Actor별 mailbox |
| STREAM session | 연결 lifecycle, packet dispatch와 Actor binding ingress |

Spot과 Actor message를 Node handler에서 다시 분배하도록 application에 요구하지 않는다. Framework service
runtime은 owner별 bounded mailbox를 drain해 등록된 handler 실행 문맥으로 연결한다. Transport readiness와
service protocol frame은 application callback에 노출하지 않는다.

## 6. 연결 관리

자동 discovery는 [location store](01-glossary.ko.md#location-store)의 [descriptor](01-glossary.ko.md#descriptor)와 lease를 사용한다. RouteMesh는 같은 MeshName의 MeshNode
descriptor를 찾고, ClientServer client는 같은 ChannelName의 전용 server descriptor를 찾는다. 두
descriptor를 서로 대신 사용하지 않는다. Object Client·Server role이나 분산 discovery를 사용하는 host는
공식 Redis location store instance를 명시적으로 등록한다.

manual peer는 endpoint 또는 expected RID와 endpoint를 application이 제공하는 연결 intent다. manual peer도
자동 discovery peer와 같은 MeshName, RID, ChannelName, generation과 security admission을 통과한다.
manual이라는 이유로 message path나 handler 의미가 달라지지 않는다.

[ClientServer Channel](01-glossary.ko.md#clientserver-channel)은 client가 업무 호출을 시작하고 server가 handler와 request reply를 제공하는 별도
service 연결이다. Node direct, Spot, Actor와 Logical Multicast가 필요하지 않은 단방향 service 경계에
사용한다. 자세한 역할과 발견 계약은
[12 ClientServer Channel](09-client-server-channel.ko.md)이 소유한다.

## 7. Framework가 숨기는 것

Framework는 transport 주소 선택, peer reconnect, multipart framing, packet codec, reply correlation과
backpressure queue를 내부에서 관리한다. application handler는 typed payload와 context를 사용하며 raw
socket 배선을 구성하지 않는다.

외부 edge gateway의 인증, quota, WAF, public API versioning과 billing은 이 framework의 계약 범위가 아니다.
