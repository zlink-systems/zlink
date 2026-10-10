# Activation and Lifetime

!!! info "What you get from this chapter"

    You can tell when each of the three kinds of Spot is created, which callbacks it receives, and
    how long a service injected into it lives.
    The code in this chapter comes from the [TicTacToe sample in the per-language example repositories](https://github.com/zlink-systems/zlink-<language>-examples/tree/main/samples/TicTacToe).

What [Spot](21-spot.en.md) created was **the Spot an application creates explicitly**. This chapter
covers how the other two kinds differ, which lifecycle callbacks each kind receives, and the
injection scope that lasts as long as the Spot.

## 1. The Kinds of Spot

All three carry an id and state and run their callbacks in order. They differ in when they are
created, in Actor membership, and in how they are closed.

| | Entry Spot | User Spot | Instance Spot |
| --- | --- | --- | --- |
| Created | By the Framework when the Object Server starts | Explicitly by the application through the spot manager | When the first message arrives for that id |
| Spot id | Issued by the Framework | Issued by the Framework on create; named by the caller on get-or-create | Named by the caller as the message's target id |
| Stable type | Not registered | Required | Required |
| Actor membership | Supported. The default place an Actor runs right after creation | Supported. Actors move in and out by join and leave | Not supported |
| Closing from the application | Not offered | Through a close call or from its own context | From its own handler or timer context |
| Where it is used | The default place for an Actor that belongs to no User Spot yet | Rooms, stages, zones | A unit that handles requests per id, such as a matchmaking worker |

<iframe class="zlink-diagram" src="/common/diagrams/34-spot-kinds-en.html" title="Three kinds of Spot — what creates them" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/34-spot-kinds-en.html" target="_blank">↗ View larger</a></p>

An Instance Spot has no create call of its own. Name the instance type on the first message and
the Framework selects an existing instance or creates one where it is needed, then **handles that
same message.**

### 1.1 Instance Spot Single Activation Scope

**Nodes sharing the location records in one Location Store do not activate the same Instance
Spot id in two places at once.** The node currently handling the Spot is its owner. The Framework
lets only the node that first secures creation rights in the location store run the factory, then
handles messages after creation commits. This guarantee relies on those location records and owner
eligibility. Deployments using independent Location Stores do not form one serial execution unit.
An owner lease, renewed periodically, establishes how long the owner is eligible to accept work.

Single activation does not mean processing continues through every failure. Some operations wait
or fail instead of creating a duplicate.

| Situation | Message processing result |
| --- | --- |
| First messages for the same id arrive at different nodes together | Only the node that secures creation rights creates the Spot. A request at a losing node ends with `Unavailable`, or retains its timeout if the original deadline has passed. Failure of a one-way send whose outbound admission has completed is recorded in diagnostics. |
| Activation is in progress at the same target | Later operations join that activation. After `Ready`, they enter the queue in arrival order; activation failure follows their original completion contracts. A call whose resolver finds creation in progress also waits for activation. |
| A `Ready` owner is forcibly stopped or its owner lease is invalid | New requests return `Unavailable`. Neither while the lease remains nor after it expires does the Framework automatically release the location record or recreate the Spot at another node on the next message. |
| Location Store connection or mutation response is lost | The Framework confirms the mutation result before assuming success; it does not create another node's instance to process the operation. `StoreFailureGrace`, the grace period retaining the existing node list, does not extend owner eligibility. A valid owner can process work until its admission deadline; after that, new message and timer callbacks and state changes are blocked. Completion and cleanup of already accepted work can continue. A request waiting for confirmation retains its original timeout, cancellation, and failure completion conditions. |
| Planned relocation is in progress | The source finishes the current callback and stops starting new callbacks. Pending work and new messages are transferred and held at the target, which starts execution after restore and owner change commit. Depending on the failure boundary, the source is preserved or the operation fails; both nodes do not execute handlers together. [Relocation](37-relocation.en.md) explains the failure results. |

`Missing` means there is no location record; only this state permits an Instance intent message to
create a new instance. An existing `Ready` instance becomes `Missing` after explicit close completes
its location release.
Recovery before `Ready` can resume within the same target lifecycle and can invoke the factory again
with the same input. A reservation whose target lifecycle ends during creation becomes `Missing`
after release. External store changes in a factory therefore need to account for repeated calls.

Single activation after manually clearing the location store or losing location and generation
records is outside this guarantee. That condition cannot be treated as normal close or owner
failure recovery.

### 1.2 Durable State and Public Generation Values

**Durable financial state, such as a shared jackpot, also needs protection in the application
store.** An Instance Spot runs its callbacks serially. It does not put another service's DB writes
or external I/O already in progress into one transaction. A jackpot balance update and the record
of its business request id belong in the same DB transaction, with a row lock or conditional update
protecting other write paths too. After timeout or connection loss, the application queries the
previous result or prevents duplicate effects with the same business request id before starting
another operation.

The public Instance Spot context exposes `ObjectGeneration` to **distinguish recreation of the
same id**.

=== "C++"

    Read it through `instance_spot_context_t::object_generation()`.

=== "C#/.NET"

    Read it through `IZLinkInstanceSpotContext.ObjectGeneration`.

=== "Java"

    Read it through `ZLinkInstanceSpotContext.objectGeneration()`.

=== "Kotlin"

    Read it through `ZLinkInstanceSpotContext.objectGeneration()`.

=== "Node/TypeScript"

    Read it through `ZLinkInstanceSpotContext.objectGeneration`.

Relocation moves the same object and does not change `ObjectGeneration`. This value cannot serve
as a fencing token that distinguishes owner changes. Fencing means the store rejects a mutation
carrying an old write authorization. When needed, the application store issues that authorization
and validates it on every write. The Framework context does not supply an owner-change token.
Ordering against earlier generation values cannot be assumed after location-store generation
records are lost either.

## 2. Lifecycle Callbacks per Kind

The names follow each language; the conditions and the order are the same.

| Callback | Entry | User | Instance | When |
| --- | :---: | :---: | :---: | --- |
| `Configure` | O | O | O | The configuration step where handlers are registered |
| `OnCreate` | X | O | X | Examines a request to create a new User Spot and decides whether to accept it. Not called when an existing Spot was found |
| `OnInitialize` | O | O | O | Initialization of the created instance. An Instance Spot receives only this, with no `OnCreate` |
| `OnClosing` | O | O | O | Before a still-valid local instance is torn down |
| `OnActorJoin` | X | O※ | X | Accepts or refuses an existing Actor that wants to enter this User Spot |
| `OnCreateActor` | O※ | X | X | Accepts or refuses a new Actor's first Entry Spot membership |
| `OnJoinedActor` | O※ | O※ | X | Tells the **receiving** Spot that the join commit finished |
| `OnLeaveActor` | O※ | O※ | X | Tells the **departing** Spot after the commit. It does not mean the Actor is gone |
| `OnDisconnectActor` | O※ | O※ | X | When the connection of an Actor in that Spot drops |

※ Only for a Spot that names an actor type and so supports Actor membership.

**Membership callbacks are split between the departing Spot and the receiving Spot.** So when an
Actor in a User Spot returns to its Entry Spot, **the Entry Spot's `OnCreateActor` and
`OnActorJoin` are not called** — returning to the Entry Spot is the default membership and has no
admission step. In both directions only the receiving side's `OnJoinedActor` and the departing
side's `OnLeaveActor` run after the commit.

!!! info "Relocation is not an arrival or a departure"

    When relocation restores an Actor into another node's Entry Spot these callbacks are not
    called. Membership is left as it is and only the execution site moves — covered by
    [Relocation](37-relocation.en.md).

## 3. Entry Spot — Created by the Framework

There is one per Object Server. It accepts or refuses Actor creation requests and handles the
lifecycle of Actors arriving and leaving.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/PlayEntrySpot.cs:doc-entry-spot"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp:doc-entry-spot"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.java:doc-entry-spot"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.kt:doc-entry-spot"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts:doc-entry-spot"
    ```

This code shows that the Entry Spot provides the lifecycle callbacks needed to admit and destroy Actors.

### 3.1 No Per-Actor State in the Entry Spot

**Keep no per-Actor state in an Entry Spot.** An Actor's state belongs to the Actor, and the Entry
Spot provides only handlers and membership callbacks. There is one Entry Spot per Object Server, so
per-Actor values accumulated here grow with the number of Actors that Object Server serves.

### 3.2 Actor Creation and Destruction Happen in the Entry Spot

The Entry Spot accepts or refuses Actor creation requests. Its context also provides the call that
destroys an Actor. An Actor in a User Spot first returns to the Entry Spot before it is destroyed;
[Actor Membership](35-actor-membership.en.md) handles that move.

The destroy call takes the current instance and does not run membership callbacks again. It clears
the Framework's registration record and the bound session route.

## 4. User Spot — Created by the Application

It is created by naming a stable type, and the id that comes back is the address for every later
call. Refuse in the create callback and the call fails, leaving no Spot behind.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Api/Handlers/CreateGameHttpHandler.cs:doc-create"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Api/Handlers/create_game_http_handler.hpp:doc-create"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/api/handlers/CreateGameHttpHandler.java:doc-create"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/api/handlers/CreateGameHttpHandler.kt:doc-create"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/Handlers/create-game-http-handler.ts:doc-create"
    ```

The id returned by this call is the address for later calls to the User Spot.

## 5. The Lifetime of an Injected Service

When the Framework activates a Spot it creates one injection scope and resolves the dependencies
of the Spot itself and of its handlers from that scope. The scope is torn down when the Spot
closes or moves to another node. So **a service registered as scoped is one instance for as long
as that Spot lives.** That differs from one created per HTTP request.

Actor handlers use a separate Actor activation scope. Different Actors share neither handlers nor
scoped dependencies. When an Actor leaves, is destroyed, or moves, the source scope is torn down
and a new one is created at the destination.

**Injecting an ORM context into the constructor of a Spot or a Spot handler causes trouble.** A
room that lives for hours keeps that context alive for hours.

| Symptom | What happens |
| --- | --- |
| Growing memory | The change tracker keeps tracking every entity it has read |
| Stale reads | Reading the same key again returns the tracked earlier instance |
| A stuck error state | A failed save leaves the context soiled for the rest of the Spot's life |

Registering the handler type as transient or singleton does not change the lifetime the Framework
sets, because the Framework creates the handler and resolves only its dependencies from the
activation scope.

**The first choice is not to reach a store from a Spot at all.** Saving and reading are requested
from a service that owns a channel handler, and the Spot owns only in-memory state and execution
order. A channel handler has a scope per dispatch, so it may take an ORM in its constructor — the
handler in [Channel Messaging](20-channel-messaging.en.md) is that place.

## 6. Related Documents

- State objects called by id — [Spot](21-spot.en.md) · [Actor](22-actor.en.md)
- What runs together — [The Execution Model](32-execution-model.en.md)
- The rules for moving an Actor between Spots — [Actor Membership](35-actor-membership.en.md)
- Moving the execution site — [Relocation](37-relocation.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
