---
title: "3. 핵심 개념 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/03-concepts.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 3. 핵심 개념

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Java Quickstart — 설치부터 첫 요청까지](../../quickstart.ko.md) | [다음: Channel 메시징](20-channel-messaging.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/03-concepts.ko.md) · [C#/.NET](../../../dotnet/guide/server/03-concepts.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/03-concepts.ko.md) · [Node/TypeScript](../../../node/guide/server/03-concepts.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    channel · Spot · Actor · STREAM · Location이 각각 무엇이고 언제 고르는지 알 수 있다.
    이 장의 코드는 [언어별 예제 저장소](https://github.com/zlink-systems/zlink-java-examples)에서 가져왔다.

ZLink framework는 **channel · spot · actor · stream · location**을 핵심 개념으로
제공한다. 뒤의 장은 이 개념들을 각각 자세히 다룬다.
아래에서 차례로 보고, 중간에 actor·spot이 다른 node로 옮겨가는
[relocation](#5-relocation--다른-node로-옮겨가기)을 함께 다룬다. 각 개념을 실제로
작성하고 운영하는 방법은 뒤의 전용 장이 소유한다.

## 1. channel — 서버 간 연결

**MeshNode**가 서버 간 연결의 기초 단위다. MeshNode 하나 위에 독립적인 두 역할을
추가한다.

- **Object role** — spot·actor를 배치하는 자리다.
  [spot](#2-spot--상태를-소유하고-순서대로-처리하는-단위),
  [actor](#3-actor--id로-식별되는-상태-객체)에서 각각
  설명한다.
- **Channel role** — request·send·publish를 주고받는 자리다. 이 문서에서 설명한다.

`ChannelName`은 그 mesh 안에서 같은 기능을 맡은 node들을 묶는 논리 이름이다.
주소(`host:port`) 대신 `"orders"` 같은 이름으로 호출 대상을 고른다. 호출자는
route client에 `ChannelName`만 넘긴다. `MeshName`은 등록에서 정해지고 호출
인자에 나타나지 않는다.

호출자는 지금 어느 node가 그 요청을 처리하는지 몰라도 된다. 주소도 node 번호도 아닌
논리 이름(`ChannelName`, spot id, actor id)만 넘기면, 그 이름이 지금 어느 node에 있든
framework가 찾아서 전달한다. 이렇게 **대상이 어디 있는지 호출자가 몰라도 되는 성질**을
위치 투명성이라 한다. channel·spot·actor 모두 이 방식으로 동작한다. 그래서 서버를
늘리거나(scale-out) 줄여도(scale-in) 호출 코드는 그대로다.

`ChannelName`으로 메시지를 전송하면 framework가 그 순간 요청을 받을 수 있는 node 중
하나를 선택해 전달한다 — 이 선택을 **select-one**이라 한다.

<iframe class="zlink-diagram" src="/common/diagrams/03-channel-select.html" title="channel — 이름으로 부르기 (select-one)" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-channel-select.html" target="_blank">↗ 크게 보기</a></p>

같은 `orders` channel을 맡은 node가 셋이면 호출마다 그중 하나가 선택된다. 호출자는
어느 node가 선택됐는지 알지 못하고, 알 필요도 없다.

MeshNode 하나에 두 역할을 함께 추가한 모양은 이렇다.

아래는 tutorial 소스 그대로다 — mesh 등록과 object role은 서버 쪽에서, channel의
호출 전용(client) role은 클라이언트 쪽에서 가져왔으므로 mesh 이름과 channel 이름은
`"orders"`·`"billing"`이 아니라 tutorial이 실제로 쓰는 `"game"`·`"profile"`이다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:mesh-register"
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:object-server"
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:channel-register"
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:channel-client-register"
```

peer 주소를 코드에 적지 않고 서버 증감을 따라가는 자동 연결은
[Location](25-location.ko.md)이 다룬다.

!!! warning "MeshName과 ChannelName은 다른 이름이다"

     하나의 mesh에 여러
> `ChannelName`을 등록할 수 있고, 서로 다른 mesh에서 같은 `ChannelName`을 사용할 수도 있다.

"channel"이라는 이름을 사용하는 등록은 아래와 같고, 소켓을 공유하는지가 다르다.

| 종류 | 소켓 |
| --- | --- |
| route mesh channel | 이미 열려있는 MeshNode 소켓을 공유한다 |
| ClientServer channel | MeshNode와 별개인 자기 소켓을 연다 |
| fanout channel | 독자적인 PUB/SUB 소켓을 연다 |

pub/sub은 다음 두 종류다. route mesh channel 위에서 Spot끼리 주고받는 **Logical Multicast**는
mesh 소켓을 그대로 사용하고, **fanout channel**은 자기 소켓으로 등록한 prefix와 publish topic이 일치하는 연결된
구독자에게 전달한다. 셋의 구조 비교와 사용법은
[Channel 메시징](20-channel-messaging.ko.md#2-channel-메시징의-구성)이 다룬다.

## 2. spot — 상태를 소유하고 순서대로 처리하는 단위

게임 방 하나, 길드 하나, 경매 물건 하나처럼 **여러 요청이 같은 상태를 동시에 건드리는
대상**이 있다.
이를 직접 만들면 그 상태를 지금 어느 process가 들고 있는지 찾아 요청을 그리로 보내는
일과, 도착한 요청들이 상태를 동시에 읽고 쓰지 않게 막는 일을 함께 구현해야 한다.
상태를 process 메모리에 두면 앞의 routing을 직접 관리해야 하고, DB나 Redis에 두면
요청마다 읽고 쓰면서 락을 잡아야 한다.

spot은 이 둘을 framework가 맡는다. 대상을 **메모리에 살아 있는 객체 하나**로 두고,
그 앞으로 온 요청을 **한 줄로 세워 차례로** 처리한다. 동시에 두 요청이 같은 상태를
건드리는 상황 자체가 생기지 않으니 락이 필요 없다.

id로 주소를 지정한다는 점이 channel과 다르다. `"orders"` channel로 전송하면 그 일을
할 수 있는 아무 node나 처리한다. 반면 `"room-42"` 같은 spot id로 요청을 보내면, 그
spot이 있는 node가 메시지를 받아 그 spot에게 전달한다. 그 node가
어디인지는 [앞에서 본](#1-channel--서버-간-연결) 위치 투명성 그대로 framework가 찾는다.

<iframe class="zlink-diagram" src="/common/diagrams/03-spot-queue.html" title="spot — 상태 소유·순서 처리" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-spot-queue.html" target="_blank">↗ 크게 보기</a></p>

spot은 MeshNode의 **Object role**에 등록한다. 같은 MeshNode의 Channel role과는
별개 표면이다.

spot은 만들어지는 시점에 따라 **Entry Spot · User Spot · Instance Spot** 세 종류로
나뉘고, 어떤 작업이 동시에 실행되는지는 **execution mode**가 정한다. 세 종류의 차이,
execution mode는 [실행 모델](32-execution-model.ko.md)이, 등록과 lifecycle은
[활성화와 수명](34-activation-lifetime.ko.md)이, timer는
[Timer와 worker](36-timer-worker.ko.md)가 다룬다.

## 3. actor — ID로 식별되는 상태 객체

actor는 **ID로 식별되는 상태 보유 객체**다. 같은 ID로 온 메시지는 늘 같은
인스턴스가 처리한다. actor는 항상 어떤 spot에 속하며, 외부 client 연결과 묶는 방법은
[다음 절](#4-stream--외부-client-연결)에서 이어진다.

<iframe class="zlink-diagram" src="/common/diagrams/03-actor-route.html" title="actor — id로 식별" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-actor-route.html" target="_blank">↗ 크게 보기</a></p>

상세는 [Actor](22-actor.ko.md)와 [Actor membership](35-actor-membership.ko.md).

## 4. stream — 외부 client 연결

stream은 모바일·게임 같은 **외부 client와의 연결 지향 양방향 채널**이다. 서버
간 [channel](#1-channel--서버-간-연결)과 달리 서버가 연결 수명·heartbeat를 관리하고,
연결 하나가 서버 측 **session** 객체에 대응한다. 연결이 끊긴 뒤 다시 연결하는 동작은
client connector가 담당한다.

session을 [actor](#3-actor--id로-식별되는-상태-객체)에 **bind**하면, 그 연결로 들어온
메시지를 session이 직접 처리하지 않고 bind된 actor로 relay한다. 반대 방향도 같아서
actor가 보내는 push는 그 actor에 bind된 session을 통해 client로 나간다.

<iframe class="zlink-diagram" src="/common/diagrams/03-stream.html" title="stream — 외부 client 연결" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-stream.html" target="_blank">↗ 크게 보기</a></p>

그래서 **연결을 받는 node와 도메인 로직을 실행하는 node를 나눌 수 있다.** session은
gateway node에 두고 actor는 다른 node에 두어도, relay 경로는 framework가 유지한다.
actor가 [relocation](#5-relocation--다른-node로-옮겨가기)으로 옮겨가도 같은 session이
새 위치로 이어진다.

상세는 [STREAM](23-stream.ko.md), session과 actor를 bind하는 방법은
[Session과 Actor 연결](24-actor-session.ko.md)이 다룬다.

## 5. relocation — 다른 node로 옮겨가기

actor나 spot이 지금 owner node를 떠나 다른 node에서 계속 실행되는 것을 relocation이라
한다. 서로 다른 두 계기로 시작된다.

actor는 spot에 속하고, spot은 node에 속한다. relocation은 이 소속 관계를 그대로
유지한 채 실행되는 node만 바뀌는 것이다.

**actor가 다른 node의 spot에 join할 때.** actor가 어떤 User Spot에 join을 요청했는데
그 spot이 다른 node에 있으면, join이 받아들여지는 순간 actor가 상태와 대기 중인 작업을
그대로 들고 그 node로 옮겨간다. application이 요청해서 일어나는 이동이다.

<iframe class="zlink-diagram" src="/common/diagrams/03-relocation.html" title="relocation — actor가 다른 node의 spot에 join" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-relocation.html" target="_blank">↗ 크게 보기</a></p>

join 호출이 지정하는 것은 **`room-42`라는 spot id뿐**이고, 대상 node를 지정하는 인자는
없다. 그 spot을 지금 어느 node가 가지고 있는지는 framework가 location store에서 찾아
actor P를 그 node로 옮긴다. 그래서 node A와 node B라는 이름은 application 코드 어디에도
나타나지 않는다. 이동이 끝나면 actor P는 node B의 `room-42` spot에 소속되어 Q·R와 같은
실행 규칙을 따른다.

**무중단 점검·배포로 host를 옮길 때.** 운영자가 한 host의 spot과 actor를 다른 host로
옮긴다. application이 개별 join을 요청하지 않아도 framework가 처리하며, 완료된 뒤
원래 host를 종료할 수 있다.

<iframe class="zlink-diagram" src="/common/diagrams/03-host-relocate.html" title="host relocate — spot과 actor를 통째로 이전" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-host-relocate.html" target="_blank">↗ 크게 보기</a></p>

상태를 보유한 서버는 그 상태 때문에 임의로 종료할 수 없어서, 점검이나 배포를 하려면
연결을 끊고 기다리게 만드는 것이 보통이다. Host
Relocate는 spot과 actor의 state를 그대로 유지하면서 다른 node로 옮겨 node A를 비운다.
호출하는 쪽은 여전히 `room-42`라는 같은 id로 요청하므로 이전 사실을 알 필요가 없다.
결과적으로 stateful 서비스를 stateless 서비스처럼 **무중단으로 교체**할 수 있다.

두 경로 모두 **같은 relocation policy**를 따른다. 이동할 때 application 상태를 어떻게
할지(옮기지 않음 · 새로 만듦 · 그대로 복원)를 spot·actor factory 등록에서 하나
고정하며, 실행 중에는 바꾸지 않는다.

policy 종류와 선택 기준은 [Relocation](37-relocation.ko.md#21-factory-등록이-고르는-이동-정책), actor join 호출과
완료 결과 수신은 [Actor membership](35-actor-membership.ko.md#23-결과를-받는-callback), 무중단 점검·배포로서의
Host Relocate와 이전 단위 구분은
[Relocate](12-operations.ko.md#3-relocate--상태를-유지한-채-다른-host로-옮기기)가 다룬다.

## 6. location — 주소 해석

Application 코드는 channel 이름 같은 논리 이름만 사용하고, 실제 peer 주소(`host:port`)는
배포 전체가 공유하는 **location store**가 해석한다. 각 서버는 시작할 때 자기 위치를
descriptor로 store에 등록하고, 호출하는 쪽은 논리 이름으로 store에서 대상을 찾아
연결한다. 서버 구성이 바뀌면 연결도 갱신된다.

사용법은 [Location](25-location.ko.md)이 다룬다.

store 없이 endpoint를 등록에 직접 지정하는 수동 연결도 지원한다 — 개발·테스트와
소규모 고정 배포에 사용한다([Channel 동작 원리](20-channel-messaging.ko.md)).
같은 MeshNode에서 두 방식을 함께 사용할 수는 없다.

> **샘플에서 보기 — [TicTacToe](../../../common/sample/tictactoe/README.ko.md).** 이 개념들이
> 한 샘플에 전부 나오는 가장 작은 예다. Play 서버의 등록 코드 한 곳에서 모두 만난다.
>
> | 개념 | TicTacToe에서 |
> | --- | --- |
> | channel | Play 서버가 독립 `tictactoe.api` ClientServer Channel로 인증 정보를 조회한다 |
> | spot | 대국 한 판이 `TicTacToeGame` spot 하나 — 두 플레이어의 수가 이 안에서 직렬 처리된다 |
> | actor | 플레이어가 actor이고, 재접속해도 같은 actor로 이어져 두던 판을 계속한다 |
> | stream | client가 API 응답의 Play STREAM endpoint에 직접 연결해 수를 두고 push를 받는다 |
> | location | Redis location store가 새 `TicTacToeGame` spot을 만들 Play node를 자동으로 고른다 — API 코드에 특정 Play node 주소가 없다 |
>
> 각 개념이 어떤 문제를 푸는지는 위에서 봤고, **함께 놓이면 어떤 모양인지**는
> 이 샘플이 보여 준다.

## 7. 작업별 시작 표면

개념을 확인했으면 다음 질문은 어느 표면에서 시작하느냐다. 시작 지점은 다음과 같고,
전부 DI나 현재 handler context에서 얻는다. **application이 transport socket이나 endpoint를
직접 고르지 않는다.**

| 하려는 일 | 시작 표면 | 지정하는 대상 |
| --- | --- | --- |
| node를 직접 지정하거나 channel 이름으로 보내기 | route client | node 직접은 MeshName과 대상 RID, channel은 ChannelName |
| Spot에 보내기 | spot client | 전역 Spot ID |
| Actor에 보내기 | actor client | 전역 Actor ID |
| User Spot 만들거나 찾기 | spot manager | Spot type과 필요하면 전역 Spot ID |
| Actor 만들거나 찾기 | actor manager | 전역 Actor ID와 Actor type |
| Logical Multicast 발행 | spot publisher client | ChannelName과 topic |
| classic pub/sub 발행 | fanout client | fanout ChannelName과 필요하면 topic |
| STREAM client에 보내거나 응답 | session client | 현재 session |

정확한 타입 이름은 언어를 따른다 — [주요 타입 사용 색인](13-interface-catalog.ko.md)에 있다.

**완료의 뜻은 두 표면에서 같다.** send 계열은 **보낼 자리가 수락**하면 반환값 없이
끝나고, request 계열은 **reply · timeout · route 오류** 중 하나로 끝난다. 어느 표면을
쓰든 같다([API에 드러나는 backpressure](33-backpressure.ko.md#4-api에-드러나는-backpressure)).

## 8. 이름 표기 규칙

가이드 전체에서 다음 표기를 일관되게 사용한다.

- Java framework public 타입은 `ZLink` prefix(대문자 `L`)를 사용한다. 예:
  `ZLinkRouteClient`, `ZLinkMessageContext`, `ZLinkFrameworkOptions`.
- annotation도 같은 prefix다. 예: `@ZLinkRequest`, `@ZLinkSpotActorSend`.
- Maven 좌표와 패키지는 `systems.zlink.*`다.
- client 측 Stream Connector는 서버 framework 아티팩트에 의존하지 않는 독립 library다.
- 하부 zlink core C API는 `zlink_*` snake_case다.

## 9. Framework가 맡는 것과 맡지 않는 것

**아래는 전부 framework가 안에서 처리한다.** application 코드에 나타나지 않는다.

| 맡는 것 |
| --- |
| transport 주소 선택과 peer 재연결 |
| multipart framing과 packet codec |
| reply correlation |
| backpressure queue |

**반대로 이것들은 이 framework의 계약 범위가 아니다.** 외부 edge에서 처리한다.

| 맡지 않는 것 |
| --- |
| 외부 client 인증과 quota |
| WAF |
| 공개 API 버전 관리 |
| 과금 |

서버 사이 통신과 실시간 상태 서버가 이 framework의 자리다. 인터넷에 직접 노출하는
edge의 정책은 그 앞단이 소유한다.

## 10. stateful 게임 노드와 HTTP 로비

ZLink로 만드는 게임 서버에는 성격이 다른 두 종류의 코드가 있다. 코드를 쓰기 전에 지금 쓰는 코드가 어느 쪽인지 먼저 정한다. AI 도구가 저지르는 실수의 대부분은 게임 노드 코드를 웹 서버처럼 만드는 데서 나온다.

| 구분 | 게임 노드 | HTTP 로비 |
|---|---|---|
| 하는 일 | 방·판·이용자 상태를 들고 메시지를 순서대로 처리한다 | 로그인, 매칭, 방 배정처럼 짧은 요청과 응답 |
| 상태 | 메모리에 있다. 주인은 Spot·Actor 하나다 | 들고 있지 않는다(무상태) |
| DB | 접속할 때 읽고, 결과만 쓴다 | 요청마다 읽고 써도 된다 |
| 템플릿 예 | 모든 템플릿의 Room·Table·Field·Channel 노드 | `match-http-lobby`, `fps-http-lobby`의 Lobby |
| 하면 안 되는 것 | 요청마다 DB를 읽는 REST식 구조 | 게임 상태(판 진행, 잔액 변화)를 로비에 두기 |

HTTP 로비는 일반 웹 서버처럼 만들어도 된다. 다만 게임 상태를 넘겨받는 순간부터는 게임 노드의 규칙을 따른다. 로비는 게임 노드에 요청을 보낼 뿐, 게임 노드의 값을 직접 바꾸지 않는다.

아래 규칙의 "흔한 실수"는 실제 게임 개발에서 AI 도구가 만든 코드에서 나온 것이다.

### 10.1 게임 노드는 REST 서버가 아니다

| 규칙 | 흔한 실수 |
|---|---|
| 이용자 상태의 주인은 세션에 묶인 이용자 Actor 하나다. 접속할 때 한 번 읽어 메모리에 두고, 요청을 처리하는 동안에는 DB를 읽지 않는다 | 슬롯 한 번 돌릴 때마다 잔액·미션·VIP를 DB에서 다시 읽고 작은 트랜잭션 약 7개로 썼다. 8 vCPU 서버에서 CPU가 2코어만 쓰이는데도 1,800명에서 p95 1.3초로 막혔다. 요청마다 계정·제재를 SELECT하고, 발행마다 수신자별 잔액을 SELECT했다 |
| DB에는 결과만 쓴다. 한 판(한 거래)의 결과는 한 트랜잭션으로 한 번 쓴다 | 수(手)마다 재전송 확인 SELECT와 기록 UPDATE가 끝나야 응답이 나가 요청마다 DB 왕복이 약 5번이었다 |
| 같은 요청인지 확인하는 것도 Spot·Actor 메모리(최근 요청 ID)에서 한다 | 위와 같음 |
| 여러 이용자가 같이 보는 값(티커, 잭팟 소식)은 노드 메모리 하나나 그 값을 소유한 Spot 하나가 들고 주기적으로 갱신한다 | 이용자마다 몇 초마다 DB를 읽어 1,800명에서 초당 약 400건이 나갔다 |
| 여러 요청이 같은 행을 갱신하면 그 값을 소유한 Spot 하나가 모아서 반영한다 | 판마다 잭팟 적립 행 하나를 UPDATE해 행 잠금에 줄이 섰다 |
| 값의 주인이 아닌 프로세스는 그 값을 직접 쓰지 않고 주인 Actor에 요청한다 | 운영 프로세스가 지갑 행을 직접 갱신해 게임 서비스가 메모리 값을 믿을 수 없었다 |
| 게임이 진행되는 동안 Spot handler는 원격 request나 DB 응답을 기다리지 않는다. handler 안의 await는 그 turn을 쥐고 있어 같은 Spot의 모든 메시지와 timer가 멈춘다 | (설계 규칙) |
| 판 결과는 Actor별로 따로 쓰지 않는다. 한 판이 끝날 때 Spot이 그 판에 참여한 모든 Actor의 결과를 한 트랜잭션으로 쓰고, 커밋 뒤 각 Actor에 결과를 알려 메모리 값을 맞춘다 | (설계 규칙) |

"그 값을 소유한 Spot 하나"는 상태를 두는 SpotWide Spot을 말한다. `PerActor` Spot은 Spot에 상태를 두지 않으므로 여러 Actor가 함께 바꾸는 값은 [실행 모델 §4](32-execution-model.ko.md#4-peractor-lane마다-하나)대로 외부 저장소에 두고, 조건부 갱신으로 지킨다.

AI가 이렇게 만드는 이유는 흔한 예제(Spring·NestJS 등)가 "리포지토리 조회 → 수정 → 저장"의 무상태 REST이기 때문이다. 게임 노드 코드에서 이 형태가 보이면 멈추고 Actor·Spot 소유 구조로 바꾼다.

판 결과 정산의 예외와 주의:

- 정산 단위는 게임 전체가 아니라 한 판(라운드)이다. 긴 게임을 끝에서만 쓰면 프로세스가 죽을 때 진행분이 모두 사라진다. 슬롯은 한 스핀이 한 판이다.
- 판 도중 나간 이용자의 결과를 판 끝까지 미룰지, 나갈 때 정산할지 게임마다 정한다. 다음 판 입장이나 다른 게임 이동과 겹치지 않게 한다.
- 바이인, 게임 안 결제처럼 그 시점에 확정해야 하는 돈은 판 끝까지 미루지 않는다. Spot이 아니라 이용자 Actor가 처리한다.
- 판 끝 정산도 Spot turn에서 기다리면 그동안 Spot이 멈춘다. 정산 중에는 새 판을 받지 않는 상태로 두거나, SpotWide User Spot·Instance Spot이면 `Yield`로 gate를 반납하고 재개한 뒤 상태를 다시 확인한다.
- 이용자 DB가 샤드로 나뉘어 한 트랜잭션이 불가능하면 판 ID를 키로 한 멱등 쓰기로 바꾼다.

### 10.2 실행 순서를 다시 만들지 않는다

| 규칙 | 흔한 실수 |
|---|---|
| Spot·Actor는 메시지를 한 번에 하나씩 처리한다. 그 상태를 lock·semaphore·mutex로 다시 지키지 않는다. 상태를 프로세스 전역(static) 저장소에 두고 키별 lock으로 직렬화하지 않는다 | 매칭·참여 상태를 프로세스 static 사전에 두고 키마다 semaphore로 직렬화했다. 노드가 둘이면 메모리가 갈라진다. 이용자 상태에 mutex를 두어 Actor 실행 순서를 이중으로 지켰다 |
| 메모리는 그 값을 쓴 쪽이 커밋 뒤에 직접 고친다. DB 커밋 훅(interceptor)으로 메모리 저장소를 고치지 않는다 | 모든 트랜잭션 커밋마다 훅이 바뀐 엔터티를 훑어 메모리 저장소를 갱신했다. 커밋을 거치지 않는 바뀜은 놓치고 모든 트랜잭션이 비용을 냈다 |
| 다른 곳에서 바뀐 값은 주인 Actor·Spot에 메시지로 알린다 | (위 두 실수의 고친 모양) |

### 10.3 메모리 상태와 장애

- 메모리 상태를 만들 때 "프로세스가 지금 죽으면 DB에 무엇이 남는가"와 재시작 정리를 함께 정한다. 먼저 돌려줄 것이 생기지 않는 구조를 고른다: 베팅은 이용자 상태 메모리에 예약하고(쓸 수 있는 돈 = 잔액 − 예약), 판 결과 트랜잭션에서 한 번에 빼고 준다. 프로세스가 죽어도 옮겨진 돈이 없으므로 돌려줄 것도 없다. 바이인처럼 판 중에 돈을 먼저 빼야 하는 게임만 시작할 때 진행 중이던 판을 취소하고 판돈을 돌려주는 정리를 둔다.
- ZLink는 Actor의 owner가 하나임을 보장하지만, 이전 노드의 늦은 DB 쓰기와 업무 단위 exactly-once는 보장하지 않는다. 게임 DB는 요청 ID unique, 조건부 갱신, 세대 토큰으로 지킨다. ObjectGeneration은 쓰기 권한 토큰이 아니다.
- 위치 저장소의 기록은 시간이 지나도, 재시작해도 남는다. 끝난 이용자의 Actor와 개인 Spot을 정리하는 소유자를 처음부터 둔다. 정리하지 않으면 기록이 쌓여 접속자가 0명이어도 읽기 비용이 늘어난다. 시험은 "이 시험이 만든 기록"만 비교한다.
- 돈·보상 결과는 DB 커밋 뒤에만 이용자에게 보낸다.
- Actor 종료와 요청이 엇갈렸을 때 다시 하는 조건은 "없어지는 중" 답과 그 경우를 뜻하는 오류 종류로만 한정한다. timeout·handler 오류에서 destroy를 부르면 살아 있는(접속 중일 수 있는) Actor를 없앤다. 그 밖의 실패는 호출자에게 돌려준다.
- 바깥 쓰기로 낡은 읽기 모델은 바뀐 부분만 낡았다고 표시하고, 그 부분을 읽는 요청만 다시 읽는다. 게임 결과마다 읽기 모델 전체를 낡았다고 표시하면 다음 요청 하나가 문장 수십 개를 다시 읽는다.

### 10.4 Actor 이동과 상태

Actor는 Spot에 들어갈 때(Join)와 노드를 옮길 때 다른 노드로 이동한다. 이동 뒤에도 남아야 하는 값은 모두 relocation adapter가 옮겨야 한다. 정책과 adapter의 언어별 이름은 각 언어의 Actor 가이드를 따른다.

- 상태가 있는 Actor는 상태를 보존하는 relocation 정책(.NET `PreserveStateWith`)으로 등록한다. 다시 만드는 정책(.NET `RecreateOnRelocation`)은 Actor를 새로 만들고 application 상태를 버린다.
- Actor의 mutable 필드는 상태 레코드 하나에 모은다. Adapter는 그 레코드 전체를 저장·복원하고, 필드를 하나씩 골라 옮기지 않는다. 다시 계산할 수 있는 캐시는 레코드 밖에 두고 복원 뒤 다시 만든다고 주석으로 적는다.
- 시험: 모든 필드를 기본값이 아닌 값으로 채운 Actor를 저장·복원해 레코드 전체가 같은지 비교한다. 필드를 추가하면 이 시험이 그 필드도 비교해야 통과하게 만든다.

### 10.5 Spot 입장 때 필요한 데이터

판단 순서:

1. 이용자의 데이터(덱, 장비, 잔액)는 이용자 Actor의 상태다. Actor가 상태를 들고 Spot에 들어오므로, Spot은 들어온 Actor의 상태를 그대로 읽는다. 메시지를 보내지 않는다. 입장 때 Actor마다 따로 처리할 일이 꼭 있으면 `defer()`로 등록한다.
2. 방 전체의 데이터(게임 설정, 테이블 규칙)는 Spot의 상태다. Spot을 만들 때 한 번 읽는다.
3. 크고 Spot에서만 쓰여 Actor가 평소 들고 있지 않는 데이터는 드물다. 정말 필요할 때만:
   - 이용자별로 따로 요청하지 않는다. 게임 시작 전에 참가자 전체 분을 Spot이 요청 하나로 가져온다(판 결과 정산과 같은 방식).
   - 그동안 Spot은 '준비 중' 상태로 두고 입장·퇴장 메시지는 계속 처리한다. 게임은 데이터가 도착한 뒤에 시작한다. SpotWide User Spot·Instance Spot이면 `Yield`로 기다리고, 재개 뒤 참가자 목록을 다시 확인한다.
   - Join 자체는 `defer()`로 등록한다. `defer()`는 Join을 handler가 끝난 뒤로 미뤄 Spot이 Join을 기다리며 멈추지 않게 한다. 데이터 로딩을 대신하지는 않는다.

### 10.6 코드 구조 — 가벼운 헥사고날

계층은 규칙과 외부 기술을 떼어 놓을 때만 둔다. 게임 규칙이 단순하거나 외부 연결이 하나뿐이면 계층을 만들지 않는다.

| 역할 | 게임 노드에서 맡는 것 | 하지 않는 것 |
|---|---|---|
| domain | 판 진행·정산 계산 같은 순수 규칙과 상태 전이. 동기 함수 | ZLink·DB·네트워크 호출, async |
| Spot·Actor handler | 상태의 주인. 메시지를 받아 domain을 부르고, 접속·판 끝에서만 port를 부른다 | 범용 DB 조회, 다른 상태 주인 만들기 |
| port | 게임이 필요로 하는 능력 하나씩: `loadPlayer`, `commitRound`, `settleBuyIn` | 범용 `Repository<T>`의 조회·저장 CRUD |
| adapter | port를 DB·외부 API로 구현. 외부 필드명을 게임 용어로 바꾼다 | 게임 규칙 판단 |

- 도메인 모델을 분리한다. 판·패·지갑 같은 게임 상태 타입은 메시지 DTO나 DB 행 타입과 따로 두고, 변환은 adapter와 handler 경계에서 한 번만 한다. 메시지·DB 스키마가 바뀌어도 게임 규칙 코드는 바뀌지 않아야 한다.
- 전달만 하는 계층을 만들지 않는다. `Handler → Service → DomainService → Entity`가 같은 호출을 넘기기만 하면 계층을 합친다.
- Spot·Actor 밖에 application service를 따로 두지 않는다. 상태의 주인이 둘이 된다.
- HTTP 로비는 무상태라 일반적인 헥사고날 배치를 써도 된다. 다만 port는 여기서도 능력 단위로 정한다.

### 10.7 세션과 로그인

- 같은 이용자의 로그인은 한 순서(그 이용자의 Actor나 이용자별 큐)에서 처리한다. Actor 밖 공유 Map에서 "확인 → 등록" 사이에 await를 두지 않는다. 동시 첫 로그인 두 개가 모두 묶여 이중 접속 방지가 깨진다.
- 같은 이용자의 Actor는 메시 전체에서 한 곳에만 있다. 이중 접속(같은 Actor가 두 곳)이 생기면 Framework나 로그인 처리의 결함이므로 원인을 고친다. 게임 시작 전 DB로 잔액을 다시 맞추는 방어는 그 수정 전까지의 임시 방편으로만 쓴다.
- 로그인 응답에 돌아갈 게임(resume)이 있으면 로비가 아니라 그 게임으로 간다.
- 정리할 때는 정리 중에 찾은 ActorRef로만, 그 이용자의 순서 안에서 한다. ActorRef의 generation은 생성 차수만 구분하고 연결(binding)은 담지 않는다.

### 10.8 핸들러와 실행 순서

- 핸들러와 HTTP 스레드에서 블로킹으로 기다리지 않는다. 트랜잭션을 연 채 다른 일을 하지 않는다(블로킹 브리지 호출, idle in transaction).
- 핸들러 안에서 자기 Spot·Actor의 수명 작업(close·destroy)을 기다리지 않는다. 같은 실행 순서를 기다리다 멈춘다.
- 한 turn 안에서 다른 Actor·Spot에 request를 보내고 답을 기다리지 않는다. 상대도 이쪽에 request를 보내면 서로 기다리며 timeout까지 멈추고, 그동안 같은 Spot의 다른 이용자 요청도 막힌다. 상태 알림은 send(답을 기다리지 않음)로 보내고, 판단은 상태를 가진 쪽이 한다.
- 잔액처럼 순서 번호(version)가 있는 값의 전파는 send로 충분하다. 상대 Actor가 없으면 만들지 않는다. 다음에 만들어질 때 DB에서 읽는다.
- 핸들러가 겨냥한 Spot 클래스와 실제 Spot 클래스는 정확히 같아야 한다. 기반 클래스를 겨냥한 핸들러는 하위 Spot의 메시지를 받지 못한다.
- 접수 응답과 완료 통지를 구분한다. 응답이 OK여도 그 뒤에 시작되는 일은 아직 끝나지 않았다.
- 개인 확인 메시지(READY 같은)로 공개 상태의 revision을 올리지 않는다. 다른 참가자의 READY가 낡은 revision으로 거절된다.

### 10.9 ZLink를 쓰는 태도

- 원격 요청의 연결 준비는 Framework가 기다린다. 게임 코드에 sleep·재시도·timeout 늘리기를 넣지 않는다.
- Framework 결함을 앱 코드로 덮지 않는다. 공개 API만 쓰는 최소 재현을 만들어 ZLink 이슈로 넘긴다.
- 구성 순서는 구성 루트 한 곳이 "ZLink 먼저, 앱 나중"으로 정한다. 그렇지 않으면 종료 때 앱이 runtime보다 늦게 멈춘다.
- 버전을 비교하기 전에 실제로 로드된 Framework·Core 버전을 확인한다.
- 위치 저장소 키의 구분자는 NUL이며 로그에서는 공백처럼 보인다. 키를 문자열로 직접 만들어 비교하지 않는다.

### 10.10 설계와 시험 습관

- 결정 하나에 판정 위치 하나. 새 판정을 만들기 전에 그 결정을 이미 내리는 곳을 찾는다. 같은 결정(예: 딜러 선택)을 여러 곳이 서로 다르게 판정하면 판마다 결과가 갈린다.
- 관측의 원본은 하나다. 조회 응답과 통지를 섞어 상태를 판단하지 않는다.
- 시험의 기대값은 명세에서 가져오고 구현에 맞추지 않는다. 무작위 결과는 입력을 고정한 단위 시험에서 정확히 확인한다.

### 10.11 점검표 — 게임 노드 코드를 합치기 전에

- [ ] 요청을 처리하는 동안 DB 읽기가 0번이다
- [ ] 한 판(한 거래)에 DB 쓰기 트랜잭션이 한 번이다
- [ ] 게임 중 Spot handler가 원격 request·DB 응답을 기다리지 않는다
- [ ] 판 결과를 Actor별이 아니라 판 단위 한 트랜잭션으로 쓰고, 커밋 뒤 Actor에 알린다
- [ ] 여러 이용자가 같이 보는 값은 노드 메모리나 소유 Spot 하나가 든다
- [ ] 이 값을 쓰는 프로세스가 주인 하나뿐이다
- [ ] Spot·Actor 상태를 지키는 lock·semaphore·static 저장소가 없다(직렬화는 Spot·Actor가 한다)
- [ ] Actor·Spot이 서로에게 request를 보내고 기다리는 순환이 없다
- [ ] 프로세스가 지금 죽으면 남는 것과 재시작 정리가 정해져 있다
- [ ] 다시 하는 조건이 "없어지는 중"뿐이고, 그 밖의 실패는 호출자에게 돌려준다
- [ ] 끝난 이용자의 Actor·개인 Spot을 정리하는 소유자가 있다
- [ ] 상태 있는 Actor는 상태 보존 relocation 정책이고, 모든 필드를 채운 저장·복원 시험이 있다
- [ ] Spot 입장 때 이용자별 원격 요청이 없다
- [ ] 핸들러에 블로킹 대기·sleep·재시도가 없다
- [ ] 돈·보상 통지는 커밋 뒤에 보낸다
- [ ] 게임 규칙이 ZLink·DB 없이 시험되고, DB 출구는 능력 단위 port이며 범용 CRUD repository가 없다

## 11. 관련 문서

- request/send/pub-sub 전체 사용법과 handler 작성·`async` 실행 모델:
  [Channel 메시징](20-channel-messaging.ko.md)
- Spot 종류·생성 시점·주입 수명: [활성화와 수명](34-activation-lifetime.ko.md)
- host 수명주기와 운영: [12-operations](12-operations.ko.md)
- 등록 지점과 계층 구조: [계층 구조와 등록 지점](01-overview.ko.md#33-계층-구조와-등록-지점)
- 전체 interface·attribute·context: [주요 타입 사용 색인](13-interface-catalog.ko.md)
- 실행 코드로 보고 싶을 때 고를 샘플: [14-samples](14-samples.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
