# Actor

This chapter quotes code from the tutorial's [`Server` and `Client` directories and its Run section](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/tutorial/README.md#run); bootstrapping and building that tree reproduces the results below.

!!! info "What you get from this chapter"

    You can create one entity by id, send messages to it, and receive answers.
    The code in this chapter runs as it stands in
    `framework/languages/<language>/tutorial`.

[Spot](21-spot.en.md) covered **a place several parties share**, such as a room or a queue. An
Actor holds **per-entity state** instead — one player, one session. Both are called by id and
process one thing at a time; what differs is that an Actor **is always inside some Spot**. This
chapter goes as far as creating one Actor and calling it; moving between rooms and membership are
covered by [Actor Membership](35-actor-membership.en.md).

## 1. Where an Actor Sits — Always Inside Some Spot

An Actor does not float on its own. Right after creation it belongs to an **Entry Spot**, and
entering a room moves it to that User Spot. The Entry Spot is the Spot the Framework creates when
the Object Server starts, and it is the default place for an Actor that belongs to no room yet.

<iframe class="zlink-diagram" src="/common/diagrams/22-actor-membership-en.html" title="An Actor lives inside a Spot" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/22-actor-membership-en.html" target="_blank">↗ View larger</a></p>

**That is why a handler receives both the Spot and the Actor.** A message sent to an actor id
runs inside the Spot that Actor belongs to now, so that Spot's state and the Actor's state are
handled in one place.

An Actor uses the same Location Store as a Spot. The registration code is the one in
[Spot](21-spot.en.md#2-the-location-store--a-prerequisite-for-registering-a-spot).

## 2. The Receiving Side — the Node That Hosts the Actor

!!! note "The receiving process"

    The code in this section goes into the process that **runs** the Actor.

### 2.1 Writing the Actor

An Actor holds its state in fields and reads its own id from the context.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Actors/Player.cs:actor-class"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/actors/player.hpp:actor-class"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/Player.java:actor-class"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/actors/Player.kt:actor-class"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Actors/player.ts:actor-class"
    ```

**An Actor is not made by a constructor.** The Framework makes it through a factory, so any
dependency it needs is taken there.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Actors/Player.cs:actor-factory"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/actors/player.hpp:actor-factory"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/PlayerFactory.java:actor-factory"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/actors/Player.kt:actor-factory"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Actors/player.ts:actor-factory"
    ```

### 2.2 Writing the Entry Spot

This is the first place an Actor lands. Unlike a room, an application does not create it; one is
registered per Object Server.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Spots/LobbySpot.cs:entry-spot"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/spots/lobby_spot.hpp:entry-spot"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/spots/LobbySpot.java:entry-spot"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/spots/LobbySpot.kt:entry-spot"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Spots/lobby-spot.ts:entry-spot"
    ```

!!! warning "Only C++ requires an admission callback"

    A C++ entry spot has to implement `on_actor_join`, and **that is the creation gate.** Leave
    it refusing and creating an Actor fails outright. The entry spot in the other four languages
    has no such callback; approving creation belongs to `onCreateActor`, which accepts by
    default.

### 2.3 Writing the Handlers

A handler receives **both the Spot and the Actor** as its first two arguments. A handler that
returns no value receives a message that arrived by `send`.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Actors/PlayerHandlers.cs:actor-send-handler"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/spots/lobby_spot.hpp:actor-send-handler"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/ChangeNicknameHandler.java:actor-send-handler"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/actors/PlayerHandlers.kt:actor-send-handler"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Actors/player.ts:actor-send-handler"
    ```

For a handler that returns a value, that value becomes the response.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Actors/PlayerHandlers.cs:actor-request-handler"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/spots/lobby_spot.hpp:actor-request-handler"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/GetPlayerHandler.java:actor-request-handler"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/actors/PlayerHandlers.kt:actor-request-handler"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Actors/player.ts:actor-request-handler"
    ```

### 2.4 Registration

Register the Entry Spot and the Actor factory on the same Object Server. The nodes that
registered the actor type become the creation candidates.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:actor-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:actor-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:actor-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:actor-register"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/main.ts:actor-register"
    ```

## 3. The Calling Side — the Node That Calls an Actor

!!! info "The calling process"

    The code in this section goes into the other process, the one that creates the Actor and
    **calls** it. It pairs with the section above.

Registration is the same as for calling a Spot — the code in
[the node that calls a Spot](21-spot.en.md#4-the-calling-side--the-node-that-calls-a-spot) is used
as it stands.

### 3.1 Creating

Unlike a Spot, **the calling side decides the id**, because a value that already exists — a
player id — is used as it stands. Call again with the same id and nothing is created; the existing
one comes back.

The tutorial `Server` first defines the `game` route mesh. A mesh name names the set of nodes
where an Actor may be placed, and `InMesh` selects that set when creating one. [The receiving side
of Channel Messaging](20-channel-messaging.en.md#32-the-receiving-side--the-node-serving-the-channel)
shows the registration code, and [Location Runtime §7](../../../common/spec/server/05-location-relocation/01-location-runtime.en.md#7-creating-an-actor-or-user-spot) defines the selection rule.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:mesh-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:mesh-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:mesh-register"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/main.ts:mesh-register"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:actor-create-call"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Client/main.cpp:actor-create-call"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:actor-create-call"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Client/src/main/kotlin/systems/zlink/tutorial/client/PlayerActorEndpoints.kt:actor-create-call"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Client/main.ts:actor-create-call"
    ```

The result tells the two apart: `created` when it was made for the first time, `existing` when
one was found.

### 3.2 Calling

Give only the actor id. The Framework finds which Spot that Actor is in now.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:actor-send-call"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Client/main.cpp:actor-send-call"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:actor-send-call"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Client/src/main/kotlin/systems/zlink/tutorial/client/PlayerActorEndpoints.kt:actor-send-call"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Client/main.ts:actor-send-call"
    ```

When you need an answer, send a request.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:actor-request-call"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Client/main.cpp:actor-request-call"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:actor-request-call"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Client/src/main/kotlin/systems/zlink/tutorial/client/PlayerActorEndpoints.kt:actor-request-call"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Client/main.ts:actor-request-call"
    ```

!!! note "The reference creation returns"

    Besides the actor id, the creation call returns a reference carrying **the exact incarnation at
    that moment and the owner route as looked up**. That reference is used when binding one
    connection to the Actor and when destroying that one incarnation specifically. An ordinary Actor
    call needs only the actor id.

## 4. What You See When You Run It

With the tutorial Server and Client running as the README's Run section specifies, send these `curl` requests to the Client's HTTP surface: each HTTP response appears on `curl` stdout, and handler records appear on the Server process's stdout or in `server.log`.

```bash
curl -X POST http://127.0.0.1:5080/players/p7 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"

curl -X POST http://127.0.0.1:5080/players/p7 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "existing"

curl -X POST http://127.0.0.1:5080/players/p7/nickname \
  -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
# 202

curl http://127.0.0.1:5080/players/p7
# {"playerId":"p7","nickname":"veteran"}
```

The point is that the second call answered `existing`. Calling again with the same id creates
nothing. The third call changed the name, and the fourth read the changed value.

## 5. The Difference from a Spot

Both are called by id and process one thing at a time. To choose between them, look at **who
decides the id** and **what it holds**. A Spot's id is issued by the Framework at creation, and it
holds a place several parties share, such as a room or a queue. An Actor's id is decided by the
calling side, and it holds the state of one entity, such as a player or a session.

Moving between rooms is covered by [Actor Membership](35-actor-membership.en.md), the membership
callbacks per kind by [Activation and Lifetime](34-activation-lifetime.en.md), and preserving state
during a move by [Relocation](37-relocation.en.md).

## 6. Related Documents

- A place several parties share — [Spot](21-spot.en.md)
- The path that calls by name — [Channel Messaging](20-channel-messaging.en.md)
- Moving and membership — [Actor Membership](35-actor-membership.en.md)
- A running version of this chapter's code — `framework/languages/<language>/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
