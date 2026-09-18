# Session and Actor

!!! info "What you get from this chapter"

    You can bind one external client's connection to one Actor, and have that Actor push a
    notification back over the same connection. The code in this chapter runs as it stands in
    `framework/languages/<language>/tutorial`.

A [STREAM](23-stream.en.md) session ends when its connection drops. A player's state has to
outlive that, and what holds it is an [Actor](22-actor.en.md). **This chapter binds the two** —
once bound, packets the session does not handle reach that Actor, and the Actor can push over
that connection.

## 1. The Problem Binding Solves

A connection and an entity have different lifetimes. When the same player drops and returns the
connection is a new one and the entity is the one that was already there. So **the connection is
made to point at its entity**, and packets after that are received by the entity.

<iframe class="zlink-diagram" src="/common/diagrams/24-actor-session-binding-en.html" title="Binding one connection to one entity" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/24-actor-session-binding-en.html" target="_blank">↗ View larger</a></p>

The session does not disappear once bound. Packets the session knows are handled by the session
first, and **only the ones it does not know** go to the Actor.

## 2. Deciding the Packets

An authentication request, its answer, and the notification the Actor pushes. The last one answers no request.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Shared/Contracts.cs:session-actor-contracts"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Shared/contracts.hpp:session-actor-contracts"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Shared/src/main/java/systems/zlink/tutorial/shared/Contracts.java:session-actor-contracts"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Shared/src/main/kotlin/systems/zlink/tutorial/shared/Contracts.kt:session-actor-contracts"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Shared/contracts.ts:session-actor-contracts"
    ```

## 3. The Receiving Side — the Node That Binds the Connection

!!! note "The receiving process"

    The code in this section goes into the process that registered the stream node in
    [STREAM](23-stream.en.md). **Actor dispatch has to be turned on at that registration** for the
    relay in this chapter to work.

### 3.1 Binding

When the client names its id, the Actor for that id is found or created and bound to this connection.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Sessions/AuthenticateHandler.cs:session-actor-bind"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/sessions/game_session.hpp:session-actor-bind"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/AuthenticateHandler.java:session-actor-bind"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/sessions/SessionHandlers.kt:session-actor-bind"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Sessions/authenticate-handler.ts:session-actor-bind"
    ```

**A client that returns with the same id binds to the Actor that is already there.** That is why creating and finding are one call.

### 3.2 Forwarding What Is Left

Packets the session did not handle are sent to the bound Actor. Before the bind there is nowhere
to send them, which is where **authentication first** is enforced.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Sessions/GameSession.cs:session-actor-relay"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/sessions/game_session.hpp:session-actor-relay"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/GameSession.java:session-actor-relay"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/sessions/GameSession.kt:session-actor-relay"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Sessions/game-session.ts:session-actor-relay"
    ```

A forwarded packet is received by that Actor's handler — the same handler written in
[Actor](22-actor.en.md). One handler is called both by a mesh call and by a STREAM relay.

### 3.3 The Actor Pushes Over That Connection

An Actor knows the connection bound to it. This is not a reply but **a notification it sends on its own.**

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Actors/PlayerHandlers.cs:actor-push"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/spots/lobby_spot.hpp:actor-push"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/ChangeNicknameHandler.java:actor-push"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/actors/PlayerHandlers.kt:actor-push"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Actors/player.ts:actor-push"
    ```

!!! warning "What happens with no bound connection differs by language"

    The same handler is also called by a mesh call, and then no connection is bound. On .NET, C++
    and Node that call **does nothing and completes.** On Java and Kotlin it **throws** — and
    synchronously, so catching the returned result does not catch it. That is why the code in those
    two languages traps the failure.

## 4. The Attaching Side — a Client Outside the Mesh

!!! info "The attaching process"

    The code in this section goes into the process that opened the connection in
    [STREAM](23-stream.en.md). It pairs with the section above.

It authenticates, then receives a notification. The second call is one-way and waits for no
answer, yet a value arrives after it — that is the notification the Actor pushed.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:session-actor-client"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:session-actor-client"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:session-actor-client"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:session-actor-client"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/StreamClient/main.ts:session-actor-client"
    ```

## 5. What You See When You Run It

```bash
dotnet run --project StreamClient/StreamClient.csproj
# connected: True
# round trip: 56ms
# bound player: p1
# pushed: speedy
```

`pushed` is the point. The client sent a rename and asked for no answer, and what came back is
**a notification the player pushed on its own.** In between, the packet passed the session,
reached the Actor, and the Actor sent back over the same connection.

## 6. After the Connection Drops — the Session Ends and the Actor Stays

The session ends and the Actor stays. A client that returns with the same id is bound to that Actor again.

The callback that tells the Actor about the drop, the rules for a binding moving to another
connection, and what happens while the Actor moves to another node are covered by
[Session and Actor](24-actor-session.en.md).

## 7. Related Documents

- Where the connection is accepted — [STREAM](23-stream.en.md)
- The entity that gets bound — [Actor](22-actor.en.md)
- Disconnect notices and relocation — [Session and Actor](24-actor-session.en.md)
- A running version of this chapter's code — `framework/languages/<language>/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
