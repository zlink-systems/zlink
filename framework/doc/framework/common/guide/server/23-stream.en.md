# STREAM

This chapter quotes code from the tutorial's [`Server` and `StreamClient` directories and its Run section](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/tutorial/README.md#run); bootstrapping and building that tree reproduces the results below.

A client that reaches a STREAM node uses its language's stream connector. [Client stream connector](../../../install.en.md#client-stream-connector) covers installation for Unity, Unreal, browser, Node, .NET, Java, and C++ clients, and the [stream connector guide](../stream-connector/README.en.md) for that language covers connection and packet handling.

!!! info "What you get from this chapter"

    You can let a program outside the mesh attach over one connection, send requests and
    receive answers. The code in this chapter runs as it stands in
    `framework/languages/<language>/tutorial`.

Every call so far ran between nodes inside the mesh. A game client or an app is outside it and
references no Framework at all. **STREAM is where such a program attaches**, and while the one
connection is open both sides send to each other. This chapter goes as far as accepting a
connection and answering one packet; binding that connection to an Actor is covered by
[Session and Actor](24-actor-session.en.md).

## 1. The Problem STREAM Solves

Channels, Spots and Actors are all **paths called from inside the mesh**. The caller has to be a
member of the mesh and to reference the Framework. A client running on a player's device meets
neither condition.

STREAM narrows that boundary to a single connection. The client needs one address and the packet
names, and it never learns how many nodes the mesh has.

<iframe class="zlink-diagram" src="/common/diagrams/23-stream-boundary-en.html" title="STREAM is the mesh outer edge" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/23-stream-boundary-en.html" target="_blank">↗ View larger</a></p>

**One session object stands for one connection.** Packets arriving on that connection are handled
one at a time in that session, so a session's fields are the place to keep the state of that
connection.

## 2. Deciding the Packets

These are no different from the contracts of a mesh call. They travel to a client outside the
mesh, though, so the names have to be the ones that client uses.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Shared/Contracts.cs:stream-contracts"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Shared/contracts.hpp:stream-contracts"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Shared/src/main/java/systems/zlink/tutorial/shared/Contracts.java:stream-contracts"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Shared/src/main/kotlin/systems/zlink/tutorial/shared/Contracts.kt:stream-contracts"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Shared/contracts.ts:stream-contracts"
    ```

## 3. The Receiving Side — the Node That Accepts Connections

!!! note "The receiving process"

    The code in this section goes into the process that **accepts** the connection.

### 3.1 Writing the Session

A session stands for one connection. Connect, disconnect, error and packet arrival all come as
callbacks, and the callbacks for one connection run in order.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Sessions/GameSession.cs:session-class"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/sessions/game_session.hpp:session-class"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/GameSession.java:session-class"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/sessions/GameSession.kt:session-class"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Sessions/game-session.ts:session-class"
    ```

**The packet-arrival callback is the gate.** Registering handlers is not enough on its own; nothing
is handled unless this callback routes the packet to them.

!!! warning "Where handlers are registered differs by language"

    .NET and Node register from the session; Java and Kotlin register alongside the stream node.
    **C++ has no registration surface at all** — every packet arrives at one callback, and telling
    them apart by name is the application's own code.

### 3.2 Writing a Packet Handler

Instead of returning a value, a handler **writes the answer itself.** A packet that arrived as a
request is answered with reply; pushing to a client that is waiting for none uses send.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Sessions/PingHandler.cs:session-handler"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/sessions/game_session.hpp:session-handler"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/PingHandler.java:session-handler"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/sessions/SessionHandlers.kt:session-handler"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Sessions/ping-handler.ts:session-handler"
    ```

### 3.3 Registration

A stream node opens a port and takes one session type. It is a registration of its own, separate from a mesh node.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:stream-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:stream-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:stream-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:stream-register"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/main.ts:stream-register"
    ```

## 4. The Attaching Side — a Client Outside the Mesh

!!! info "The attaching process"

    The code in this section goes into the process that **attaches** from outside the mesh. It
    pairs with the section above.

This process references no Framework. It references one separately published **connector** and nothing else.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:stream-client"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:stream-client"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:stream-client"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:stream-client"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/StreamClient/main.ts:stream-client"
    ```

!!! warning "The Node connector attaches over a WebSocket"

    The connector in the other four languages speaks TCP and names a `tcp://` endpoint. The Node
    connector is published for the browser and speaks WebSocket, so **both the stream node and the
    client name a `ws://` endpoint.**

## 5. What You See When You Run It

With the tutorial Server running as the README's Run section specifies, start StreamClient with the command below: the request it sends and response it receives appear on the StreamClient process's stdout, while connection and handler records appear on the Server process's stdout or in `server.log`.

```bash
dotnet run --project StreamClient/StreamClient.csproj
# connected: True
# round trip: 56ms
```

`connected` turning true means the connection was established, and the round trip is measured by
the server echoing back the instant the client sent. Those two lines are the outside and the
inside joined.

## 6. The Difference from a Mesh Call — the Connection Is the Target

In a mesh call the caller is a member of the mesh and the target is a name or an id. In STREAM
**the connection itself is the target.** Nothing chooses among clients, and the server can send
first.

When the connection drops that session ends. To carry state beyond one connection, bind the
connection to an Actor — covered by [Session and Actor](24-actor-session.en.md).

## 7. Related Documents

- Binding a connection to an entity — [Session and Actor](24-actor-session.en.md)
- State objects called by id — [Spot](21-spot.en.md) · [Actor](22-actor.en.md)
- The whole session lifecycle and its options — [STREAM](23-stream.en.md)
- A running version of this chapter's code — `framework/languages/<language>/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
