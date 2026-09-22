# ZoneWorld 따라 읽기

!!! info "이 장을 읽고 나면"

    ZoneWorld 샘플을 편집기에 열고, player가 월드에 입장해 zone 경계를 넘어 다른 node로 옮겨지는
    경로와, 운영 콘솔의 공지·점검이 모든 node에 도달하는 경로를 코드에서 따라갈 수 있다. 이 장의
    코드는 `framework/languages/<언어>/samples/ZoneWorld`에서 그대로 실행된다.

[샘플 고르기](14-samples.ko.md#9-zoneworld--zone-분할-mmorpg와-운영-관제-구축)가 이 샘플이 무엇을
보여 주는지 소개했다. 이 장은 그 소개 다음에 읽는 자리다 — 역할과 코드 위치, 주요 시나리오의
메시지 흐름, 각 흐름에 등장하는 framework 기능과 그것을 설명하는 장을 소스가 놓인 순서대로
따라간다. 이 장에는 계약을 소유하는 스펙 문서가 없다. 요구사항, 메시지 계약과 검증 기준은
[ZoneWorld 시나리오](../../../common/sample/zoneworld/README.ko.md)가 소유하며, 이 장은 그것을
다시 적지 않는다.

## 1. 이 샘플이 보여 주는 것

월드는 zone으로 나뉘고, 각 zone은 id를 가진 User Spot이다. 어느 ZoneNode가 어느 zone을 맡는지는
설정이 아니라 Location Store의 placement가 정한다. player는 id를 가진 Actor이며, zone 경계를 넘는
것은 인접 zone Spot에 join하는 것이다 — owner가 다르면 framework가 Actor를 옮기고, client의
연결은 그대로 유지된다. 운영 콘솔은 node 목록 없이 공지와 점검을 보낸다.

<iframe class="zlink-diagram" src="/common/diagrams/14-zoneworld.html" title="ZoneWorld 샘플 토폴로지" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-zoneworld.html" target="_blank">↗ 크게 보기</a></p>

이 장이 따라가는 흐름은 입장과 bind → 이동과 경계 join → relocation과 join 완료 → zone tick의
push와 경계 snapshot → 운영 fanout과 node 관찰 순서다. "여러 node에 무언가를 한다"가 상황마다
다른 표면을 쓴다는 것이 이 샘플의 주제이고, 표면별 코드가 그 순서로 나온다.

## 2. 역할과 코드 위치

| 역할 | process 수 | 소유하는 것 | 코드 위치 |
| --- | ---: | --- | --- |
| Gateway | 1 | game STREAM, player Actor bind, relay와 push | `Server/Gateway` |
| ZoneNode | 2 | Entry Spot, zone Spot, player Actor, bot timer, local report | `Server/ZoneNode` |
| Ops | 1 | ops STREAM, runtime event 수집, fanout publish, maintenance store | `Server/Ops` |
| Client | 브라우저 | game 화면과 ops 콘솔 | `Client` |

이동 규칙과 zone 좌표는 `Server/ZoneNode/Domain`에 있고 framework type을 참조하지 않는다. 메시지는
`Shared`의 JSON 계약이 정한다.

## 3. 서버 구성 — mesh, capacity와 fanout

ZoneNode는 Entry Spot, player Actor factory와 zone Spot factory를 등록한다. zone Spot factory는
node당 수용량을 선언한다 — 모든 ZoneNode가 모든 zone을 요청하지만, 수용량 때문에 각 node는 일부만
소유하게 된다.

=== "C#/.NET"

    `Server/ZoneNode/Program.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Program.cs:doc-zw-node-register"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-node-register"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java:doc-zw-node-register"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt:doc-zw-node-register"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/zone-node-module.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/zone-node-module.ts:doc-zw-node-register"
    ```

player Actor factory는 상태를 보존하는 adapter를 지정하고, zone Spot factory는 relocation을 끈다.
mesh와 별도로 classic fanout channel의 subscriber를 등록한다.

=== "C#/.NET"

    `Server/ZoneNode/Program.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Program.cs:doc-zw-fanout-subscribe"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-fanout-subscribe"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java:doc-zw-fanout-subscribe"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt:doc-zw-fanout-subscribe"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/zone-node-module.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/zone-node-module.ts:doc-zw-fanout-subscribe"
    ```

Ops 쪽은 같은 fanout channel의 publisher다.

=== "C#/.NET"

    `Server/Ops/Program.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/Ops/Program.cs:doc-zw-fanout-publisher"
    ```

=== "C++"

    `Server/Ops/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/Ops/main.cpp:doc-zw-fanout-publisher"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java:doc-zw-fanout-publisher"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt:doc-zw-fanout-publisher"
    ```

=== "Node/TypeScript"

    `Server/Ops/ops-module.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/Ops/ops-module.ts:doc-zw-fanout-publisher"
    ```

capacity로 정하는 placement는 [활성화와 수명](34-activation-lifetime.ko.md)이, fanout channel은
[Channel 메시징](20-channel-messaging.ko.md#5-fanout-channel)이 다룬다.

## 4. 입장 — bind와 첫 zone join

브라우저가 Gateway에 접속해 join을 보내면 session은 `PlayerId`로 Actor를 만들거나 찾아 현재
session에 묶고, join packet을 그 Actor로 relay한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-join-move.html" title="입장과 같은 zone 이동 — JoinWorld · Move" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-join-move.html" target="_blank">↗ 크게 보기</a></p>

=== "C#/.NET"

    `Server/Gateway/Infrastructure/ZLink/Sessions/PlayerSession.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/Gateway/Infrastructure/ZLink/Sessions/PlayerSession.cs:doc-zw-session-bind"
    ```

=== "C++"

    `Server/Gateway/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/Gateway/main.cpp:doc-zw-session-bind"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/gateway/GameSession.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/gateway/GameSession.java:doc-zw-session-bind"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/gateway/GameSession.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/gateway/GameSession.kt:doc-zw-session-bind"
    ```

=== "Node/TypeScript"

    `Server/Gateway/player-session.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/Gateway/player-session.ts:doc-zw-session-bind"
    ```

Actor는 Entry Spot에서 시작한다. Entry Spot의 handler는 좌표로 zone을 정하고 그 zone Spot에
join을 예약한다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneEntrySpot.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneEntrySpot.cs:doc-zw-entry-join"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-entry-join"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/handlers/EntryZoneJoinHandler.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/handlers/EntryZoneJoinHandler.java:doc-zw-entry-join"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-entry-join"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts:doc-zw-entry-join"
    ```

join은 handler가 끝난 뒤 실행되고, 결과는 Actor의 completion callback으로 도착한다. 입장 응답은
그 callback에서 보낸다 — 응답이 왔다는 것은 target zone의 admission까지 끝났다는 뜻이다. 묶기는
[Session과 Actor 연결](24-actor-session.ko.md)이, 예약의 규칙은
[Actor membership](35-actor-membership.ko.md#2-예약-등록--handler가-끝난-뒤에-실행된다)이 다룬다.

## 5. 이동 — 같은 zone 안과 경계 너머

이동 요청은 묶인 Actor로 relay되어 zone Spot의 actor send handler에 도착한다. 판정은 domain이
하고, 결과에 따라 거부, 같은 zone 안의 이동, zone 변경으로 나뉜다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/Handlers/PlayerMoveHandlers.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/Handlers/PlayerMoveHandlers.cs:doc-zw-move"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-move"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java:doc-zw-move"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-move"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts:doc-zw-move"
    ```

같은 zone 안이면 Actor가 좌표를 갱신하고 zone Spot에 사본 갱신을 보낸다. zone이 바뀌면 target zone
Spot에 join을 예약한다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/Handlers/PlayerMoveHandlers.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/Handlers/PlayerMoveHandlers.cs:doc-zw-zone-change"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-zone-change"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java:doc-zw-zone-change"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-zone-change"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts:doc-zw-zone-change"
    ```

application은 target의 owner가 같은 node인지 다른 node인지 구분하지 않는다. 같은 node면
membership만 바뀌고, 다른 node면 framework가 join 안에서 Actor를 옮긴다. 이동 중 이전 owner에
도착한 메시지는 target으로 전달된다.

## 6. relocation과 join 완료

owner가 바뀔 때 Actor의 application 상태는 factory에 지정한 adapter가 직렬화한다. 좌표, zone, bot
방향과 pending join만 담고, queue와 timer는 framework가 옮긴다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-relocation.html" title="경계 이동과 relocation" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-relocation.html" target="_blank">↗ 크게 보기</a></p>

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Actors/PlayerActorFactory.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Actors/PlayerActorFactory.cs:doc-zw-actor-capture"
    ```

=== "C++"

    `Server/ZoneNode/player_actor_relocation_adapter.hpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/player_actor_relocation_adapter.hpp:doc-zw-actor-capture"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/actors/PlayerActorRelocationAdapter.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/actors/PlayerActorRelocationAdapter.java:doc-zw-actor-capture"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-actor-capture"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor-relocation-adapter.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor-relocation-adapter.ts:doc-zw-actor-capture"
    ```

target zone Spot의 admission이 최종 판정자다. 그 node가 점검 중이면 join을 거부하고, 아니면 pending
으로 기록한 뒤 수락한다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneSpot.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneSpot.cs:doc-zw-admission"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-admission"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java:doc-zw-admission"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-admission"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/zone-spot.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/zone-spot.ts:doc-zw-admission"
    ```

join의 결과는 Actor의 completion callback에 도착한다. callback은 operation id로 중복을 걸러내고,
수락이면 입장 응답을, 거부면 실패 notify를 bound session으로 보낸다. 거부된 이동의 좌표는 그대로다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Actors/PlayerActor.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Actors/PlayerActor.cs:doc-zw-join-completed"
    ```

=== "C++"

    `Server/ZoneNode/player_actor_relocation_adapter.hpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/player_actor_relocation_adapter.hpp:doc-zw-join-completed"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/actors/PlayerActor.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/actors/PlayerActor.java:doc-zw-join-completed"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-join-completed"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.ts:doc-zw-join-completed"
    ```

옮겨도 남는 것과 adapter의 역할은 [Relocation](37-relocation.ko.md)이, 이동 중 연결이 유지되는
규칙은 [Session 묶음의 동작 원리](39-session-binding.ko.md)가 다룬다.

## 7. zone tick — push와 경계 snapshot

zone Spot은 timer로 tick을 돌며 자기 zone과 인접 snapshot으로 state notify를 만든다. notify는
각 player Actor로 보내지고, Actor의 handler가 bound session으로 push한다. bot은 bound session이
없는 같은 type의 Actor다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/Handlers/PlayerMoveHandlers.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/Handlers/PlayerMoveHandlers.cs:doc-zw-state-push"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-state-push"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/actors/PlayerActor.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/actors/PlayerActor.java:doc-zw-state-push"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-state-push"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.ts:doc-zw-state-push"
    ```

경계 근처 상태는 인접 zone별 topic으로 publish한다. 보내는 zone과 받는 zone을 topic 이름에 모두
넣으므로 무관한 zone에는 전달되지 않는다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneSpot.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneSpot.cs:doc-zw-border-publish"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-border-publish"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java:doc-zw-border-publish"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-border-publish"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/zone-spot.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/zone-spot.ts:doc-zw-border-publish"
    ```

각 zone Spot은 자기 인접 zone에서 오는 topic만 구독한다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneSpot.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneSpot.cs:doc-zw-border-subscribe"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-border-subscribe"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/handlers/BorderSubscriptionHandlers.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/handlers/BorderSubscriptionHandlers.java:doc-zw-border-subscribe"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-border-subscribe"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Handlers/zone-runtime-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/zone-runtime-handlers.ts:doc-zw-border-subscribe"
    ```

timer는 [Timer와 worker](36-timer-worker.ko.md#1-timer--주기-실행)가, topic으로 대상을 고르는
Logical Multicast는 [Channel 동작 원리](30-channel-patterns.ko.md#5-pubsub의-형태)가 다룬다.

## 8. 운영 — fanout과 node 관찰

Ops는 공지와 점검 desired state를 fanout으로 publish한다. publisher는 node 목록을 갖지 않는다 —
node를 추가해도 이쪽 코드는 바뀌지 않는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-ops.html" title="Ops 관찰, announce와 maintenance" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-ops.html" target="_blank">↗ 크게 보기</a></p>

=== "C#/.NET"

    `Server/Ops/Infrastructure/ZLink/WorldOperationsAdapter.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/Ops/Infrastructure/ZLink/WorldOperationsAdapter.cs:doc-zw-ops-publish"
    ```

=== "C++"

    `Server/Ops/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/Ops/main.cpp:doc-zw-ops-publish"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/OpsSession.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/OpsSession.java:doc-zw-ops-publish"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/OpsSession.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/OpsSession.kt:doc-zw-ops-publish"
    ```

=== "Node/TypeScript"

    `Server/Ops/ops-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/Ops/ops-handlers.ts:doc-zw-ops-publish"
    ```

각 ZoneNode의 subscriber는 자기 `NodeId` 몫만 로컬 정책에 적용한다. 이 cache가 오래되어도 최종
판정은 앞 절의 target admission이 한다.

=== "C#/.NET"

    `Server/ZoneNode/Infrastructure/ZLink/Handlers/FanoutSubscribers.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/FanoutSubscribers.cs:doc-zw-maintenance-subscriber"
    ```

=== "C++"

    `Server/ZoneNode/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-zw-maintenance-subscriber"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/handlers/NodeMaintenanceSubscriber.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/handlers/NodeMaintenanceSubscriber.java:doc-zw-maintenance-subscriber"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-maintenance-subscriber"
    ```

=== "Node/TypeScript"

    `Server/ZoneNode/Infrastructure/ZLink/Handlers/node-channel-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/node-channel-handlers.ts:doc-zw-maintenance-subscriber"
    ```

node가 등록되고 연결됐는지는 요청이 아니라 runtime status의 관찰로 안다. Ops는 mesh의 status를
observe해 Ready peer 집합의 변화를 node 연결 상태로 옮긴다.

=== "C#/.NET"

    `Server/Ops/Infrastructure/ZLink/Monitoring/OpsEventHandlers.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/Ops/Infrastructure/ZLink/Monitoring/OpsEventHandlers.cs:doc-zw-observe-peers"
    ```

=== "C++"

    `Server/Ops/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/Ops/main.cpp:doc-zw-observe-peers"
    ```

=== "Java"

    `Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/NodeLivenessObserver.java`

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/NodeLivenessObserver.java:doc-zw-observe-peers"
    ```

=== "Kotlin"

    `Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/NodeLivenessObserver.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/NodeLivenessObserver.kt:doc-zw-observe-peers"
    ```

=== "Node/TypeScript"

    `Server/Ops/ops-runtime-events.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/Ops/ops-runtime-events.ts:doc-zw-observe-peers"
    ```

fanout의 형태는 [Channel 동작 원리](30-channel-patterns.ko.md#5-pubsub의-형태)가, runtime status와
drain은 [운영과 lifecycle](12-operations.ko.md)이 다룬다.

## 9. 실행과 검증

runner 하나가 Redis 컨테이너, 서버 process와 브라우저 시나리오를 함께 띄운다.

=== "C#/.NET"

    ```bash
    framework/languages/dotnet/samples/ZoneWorld/run_sample.sh
    ```

=== "C++"

    ```bash
    framework/languages/cpp/samples/ZoneWorld/run_sample.sh
    ```

=== "Java"

    ```bash
    framework/languages/java/samples/java/ZoneWorld/run_sample.sh
    ```

=== "Kotlin"

    ```bash
    framework/languages/java/samples/kotlin/ZoneWorld/run_sample.sh
    ```

=== "Node/TypeScript"

    ```bash
    framework/languages/node/samples/ZoneWorld/run_sample.sh
    ```

브라우저 시나리오는 경계 이동, relocation 뒤에도 유지되는 연결, 공지 도달과 점검 중 admission 거부를
확인한다. 확인 항목과 시나리오 id는
[ZoneWorld 시나리오](../../../common/sample/zoneworld/README.ko.md#9-client-self-check)가 정한다.

## 10. 관련 문서

- 다른 샘플과의 비교와 선택 기준: [샘플 고르기](14-samples.ko.md)
- 요구사항, 메시지 계약과 완료 기준: [ZoneWorld 시나리오](../../../common/sample/zoneworld/README.ko.md)
- Actor가 다른 node의 room에 join하는 같은 경로를 가장 작은 구성으로 본 것:
  [TicTacToe 따라 읽기](51-tictactoe.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
