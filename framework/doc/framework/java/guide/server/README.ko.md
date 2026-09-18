---
title: "가이드 홈 · Java"
---

# ZLink Framework Java — 사용 가이드

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/README.ko.md) · [C#/.NET](../../../dotnet/guide/server/README.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/README.ko.md) · [Node/TypeScript](../../../node/guide/server/README.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

**실시간 메시징이 중요한 서버 시스템**을 여러 프로세스로 나눠 만드는 Java
애플리케이션 프레임워크다. Spring Boot 위에 그대로 들어가므로 별도 런타임으로 옮겨갈
필요가 없다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:mesh-register"
```

handler 하나를 등록하면 메시지 디코딩·routing·인코딩은 framework가 처리한다.

---

## 만드는 시스템

여러 서버 프로세스가 역할을 나눠 협력하고, 상태 변화를 실시간으로 client에 전달해야
하는 시스템에 맞게 설계됐다.

| 도메인 | 핵심 시나리오 |
|--------|--------------|
| **실시간 게임** | room 생성 → player 입장 → 게임 상태 갱신 → client push |
| **고객 지원 채팅** | 대화 개설 → 상담원 배정 → 메시지 중계 → 대화 상태 push |
| **주문 워크플로** | 주문 접수 → 단계별 처리 → 상태 변경 → client 알림 |
| **배송·배차** | 배차 요청 → 수행자 배정·수락 → 상태 추적 → 실시간 push |

공통 구조는 역할별 서버 프로세스가 typed 메시지로 통신하고, client는 실시간 연결로
상태 변화를 받는 것이다.

<iframe class="zlink-diagram" src="/common/diagrams/guide-topology.html"
        title="역할별 서버가 typed 메시지로 통신하고, client는 STREAM으로 받는다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-topology.html" target="_blank">↗ 크게 보기</a></p>

---

## 핵심 기능

### Channel 메시징 — 서버 간 typed 요청-응답

channel은 서버 사이의 통신 경로에 붙인 이름이다. 한쪽이 channel 이름으로 요청을 보내면
반대편이 처리해 응답한다. 직렬화(JSON · MessagePack · Protobuf)는 framework가 처리한다.

보내는 쪽이다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:channel-request-call"
```

받는 쪽이다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/channel/GetPlayerProfileHandler.java:channel-request-handler"
```

request-reply 외에 fanout(pub/sub)과 route mesh(주소 라우팅) 패턴도 제공한다.
[Channel 메시징 →](20-channel-messaging.ko.md)

---

### Spot — 상태 단위를 lock 없이 관리

Spot은 게임 room, 지원 대화, 주문 처리 단위처럼 **상태 영역 하나**와 그 참가자를 묶는
실행 단위다. 한 Spot 안에서 일어나는 일 — 참가자 packet, timer, 입장과 퇴장 — 은 모두
**직렬로** 처리된다. lock 없이 상태에 접근하며, 비동기 처리를 사용해도 같은 Spot에 두
요청이 겹치지 않는다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/spots/GameRoom.java:spot-class"
```

배정을 담당하는 entry spot(node마다 하나)과 상태 본체인 room spot(단위마다 하나)으로
나뉜다. 주기 작업은 timer로 등록한다.
[Spot →](21-spot.ko.md)

---

### STREAM과 Actor — client 실시간 연결

client의 실시간 양방향 연결을 **STREAM**이라 하고, 연결 하나를 대표하는 서버 쪽 객체가
**Actor**다. client가 접속하면 session이 Actor를 만들고, Actor는 Spot에 입장해 상태 처리에
참가한다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/GameSession.java:session-actor-relay"
```

client 쪽 접속은 별도 산출물인 stream connector가 담당한다.
[STREAM →](23-stream.ko.md) · [Session과 Actor 연결 →](24-actor-session.ko.md)

---

### Location — endpoint를 코드에 적지 않는다

같은 역할의 서버 여러 대가 떠 있을 때 어느 쪽으로 연결할지 주소를 코드에 적지 않는다.
공용 location store가 주소를 관리하고, 각 서버는 id로 지금 있는 node를 조회한다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:location-store"
```

[Location →](25-location.ko.md)

---

### Spring Boot가 제공하는 것

DI container, configuration, HTTP endpoint, logging은 Spring Boot의 것을 그대로 사용한다.
ZLink Framework는 그 위에 handler와 mesh를 등록하므로, 같은 프로세스 안에서 REST
endpoint와 실시간 연결이 함께 동작한다.

---

## 목차

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [개요](01-overview.ko.md) | 무엇을 푸는가, 기존 방식과 무엇이 달라지는가 |
| 2 | [퀵스타트](../../quickstart.ko.md) | 설치, 두 process가 서로 호출하는 최소 project, 첫 실행 점검 |
| 3 | [핵심 개념](03-concepts.ko.md) | channel·Spot·Actor·session이 각각 무엇인가 |
| 4 | [Channel 메시징](20-channel-messaging.ko.md) | 이름으로 부르는 경로 — 등록과 호출 |
| 5 | [Spot](21-spot.ko.md) | 여럿이 함께 사용하는 자리를 id로 만들고 부르기 |
| 6 | [Actor](22-actor.ko.md) | 개체 하나를 id로 만들고 부르기 |
| 7 | [STREAM](23-stream.ko.md) | mesh 밖의 client가 연결 하나로 붙기 |
| 8 | [Session과 Actor 연결](24-actor-session.ko.md) | 연결 하나를 Actor 하나에 묶기 |
| 9 | [Location](25-location.ko.md) | id로 지금 있는 node를 조회하기 |
| 10 | [모니터링](26-monitoring.ko.md) | 기능별 가이드 자리 표시 — 본문은 아직 없다 |
| 11 | [실행 모델](32-execution-model.ko.md) | 두 queue, 직렬화 범위, 실행권 |
| 12 | [Backpressure](33-backpressure.ko.md) | 처리보다 도착이 빠를 때와 영향을 주는 옵션 |
| 13 | [활성화와 수명](34-activation-lifetime.ko.md) | 종류별 생성 시점, lifecycle callback, 주입 수명 |
| 14 | [Actor membership](35-actor-membership.ko.md) | Spot 사이 이동, 예약과 상한 |
| 15 | [Timer와 worker](36-timer-worker.ko.md) | 주기 실행, 줄 밖 실행, 실행권 반납 |
| 16 | [Relocation](37-relocation.ko.md) | 옮겨도 남는 것, adapter, 이동 단위 |
| 17 | [Channel 동작 원리](30-channel-patterns.ko.md) | 패턴 차이, 대상 선택, pub/sub, 연결과 discovery |
| 18 | [Handler와 메시지 처리](31-handler-dispatch.ko.md) | 등록 변형, filter, codec, handler 종류 |
| 19 | [STREAM의 동작 원리](38-stream-boundary.ko.md) | 등록 검증, 오류 귀속, 응답 token, 실행 방식 |
| 20 | [Session 묶음의 동작 원리](39-session-binding.ko.md) | 묶는 개수, 경로 갱신, 끊김 통지, 실패 |
| 21 | [ZLink를 어디에 쓰나](17-alternative.ko.md) | 사용처, 문제 신호, 기술 선택 경계, 라이선스 |
| 22 | [운영과 lifecycle](12-operations.ko.md) | 런타임 메트릭, relocate, drain, readiness 연결 |
| 23 | [Options](16-options.ko.md) | 옵션 목록, 기본값과 바꾸는 시점 |
| 24 | [샘플 고르기](14-samples.ko.md) | 어떤 샘플을 먼저 볼지 고르고 실행하는 방법 |
| 25 | [Bingo 따라 읽기](50-bingo.ko.md) | 인증·매칭·room·timer·multicast·cleanup을 코드 순서로 |
| 26 | [TicTacToe 따라 읽기](51-tictactoe.ko.md) | 수동 연결·수동 등록, 다른 node로의 Actor join |
| 27 | [SupportChat 따라 읽기](52-supportchat.ko.md) | session 하나에 Actor 여럿, metadata relay, idle timer |
| 28 | [DeliveryDispatch 따라 읽기](53-deliverydispatch.ko.md) | one-way send, deadline 기록과 재배정, 고객 push |
| 29 | [ShoppingMall 따라 읽기](54-shoppingmall.ko.md) | owner Instance Spot, replay·다음 단계·expected version |
| 30 | [GameQuest 따라 읽기](55-gamequest.ko.md) | player별 owner, best-effort push와 보정 |
| 31 | [ZoneWorld 따라 읽기](56-zoneworld.ko.md) | capacity placement, 경계 join과 relocation, fanout·관찰 |
| 32 | [E2E 테스트](15-e2e-testing.ko.md) | client library로 시스템 전체를 검증하기 |
| 33 | [주요 타입 사용 색인](13-interface-catalog.ko.md) | 계약 인터페이스를 검증 코드로 색인 |
| 34 | [모니터링](26-monitoring.ko.md) | 재작성 대기 — 상태 snapshot과 진단 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다. 읽는 순서는 이 표가 정한다.

01 · 11 · 13 · 16장은 설치 방법과 표면 이름이 언어마다 달라 Java 전용으로
따로 작성되어 있다. 위 표의 링크에서 각 장을 바로 연다.

## 다이어그램 읽는 법

이 가이드의 다이어그램은 같은 시각 언어를 사용한다 — 색이 곧 개념이다.

<iframe class="zlink-diagram" src="/common/diagrams/guide-element-kinds.html"
        title="구성도에 나오는 다섯 가지" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-element-kinds.html" target="_blank">↗ 크게 보기</a></p>

여러 장이 같은 토폴로지를 그리며, 장마다 확대하는 위치만 바뀐다.

## 관련 문서

- 공개 계약: [Java 공개 계약](../../../common/spec/server/languages/java/README.ko.md)
- 언어 중립 의미: [공통 스펙](../../../common/README.ko.md)
- client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/README.ko.md)
