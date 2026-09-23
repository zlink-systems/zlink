# Reading Along: SupportChat

!!! info "What you get from this chapter"

    You can open the SupportChat sample in an editor and follow a message from the customer's
    request to open a conversation, through agent assignment, to the conversation closing, through
    the code of each server it passes. The code in this chapter comes from the [SupportChat sample in the per-language example repositories](https://github.com/zlink-systems/zlink-<language>-examples/tree/main/samples/SupportChat).

[Picking a Sample](14-samples.en.md#5-supportchat--building-a-live-chat-support-system) introduces
this sample. This chapter explains its roles and code locations, main message flows, and run
verification in source order. See the [SupportChat scenario](../../../common/sample/supportchat/README.en.md)
for requirements and message contracts.

## 1. What This Sample Demonstrates

The customer and the agent each keep a single STREAM connection to the Session server. API handles
authentication, and the conversation state is owned by a conversation Spot on Support. One agent
handles several conversations at once, so the agent's single connection has a roster Actor and one
conversation Actor per conversation bound to it. The agent sends through each room's Actor handle,
and Session selects the relay target from the packet's Actor slot.

<iframe class="zlink-diagram" src="/common/diagrams/14-supportchat-en.html" title="SupportChat sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-supportchat-en.html" target="_blank">↗ View larger</a></p>

The flow this chapter follows is authentication and the identity Actor binding → opening a
conversation and assignment → the agent joining the conversation → chat relayed by Actor slot → the idle
timer and closing.

## 2. Roles and Where the Code Lives

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| Session | 1 | STREAM connections, authentication and join packets, Actor binding and Actor slot relay | `Server/Session` |
| API | 1 | Token verification, the request to create a conversation Spot | `Server/Api` |
| Support | 1 | Entry Spot, conversation Spot, identity, roster and conversation Actors, assignment and pushes | `Server/Support/Infrastructure` |
| Client | 1 | The customer and agent scenarios, with self-checks | `Client` |

Participants, `MessageSeq`, typing and the idle/close transitions live in `Server/Support/Domain` and
reference no framework type. The agent capacity decision belongs to `Server/Support/Application`.
Messages are set by the JSON contract in `Shared`.

## 3. Server Configuration

Session calls objects in the Support mesh, calls the API channel, and receives STREAM connections.

=== "C#/.NET"

    `Server/Session/SessionServerHostFactory.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/SessionServerHostFactory.cs:doc-sc-session-register"
    ```

=== "C++"

    `Server/Session/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Session/main.cpp:doc-sc-session-register"
    ```

=== "Java"

    `Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/Program.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/Program.java:doc-sc-session-register"
    ```

=== "Kotlin"

    `Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/SessionApplication.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/SessionApplication.kt:doc-sc-session-register"
    ```

=== "Node/TypeScript"

    `Server/Session/supportchat-session-module.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Session/supportchat-session-module.ts:doc-sc-session-register"
    ```

Support registers the Entry Spot, the Actor factory and the conversation Spot factory.

=== "C#/.NET"

    `Server/Support/SupportServerHostFactory.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/SupportServerHostFactory.cs:doc-sc-support-register"
    ```

=== "C++"

    `Server/Support/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Support/main.cpp:doc-sc-support-register"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/Program.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/Program.java:doc-sc-support-register"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/SupportApplication.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/SupportApplication.kt:doc-sc-support-register"
    ```

=== "Node/TypeScript"

    `Server/Support/supportchat-support-module.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/supportchat-support-module.ts:doc-sc-support-register"
    ```

## 4. Authentication and the Identity Actor

The first packet the client sends is an authentication request. The session splits packets by name
into authentication, conversation join and everything else.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-auth-join-en.html" title="Authentication, conversation creation and agent join" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-auth-join-en.html" target="_blank">↗ View larger</a></p>

=== "C#/.NET"

    `Server/Session/Sessions/SupportChatSession.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-session-dispatch"
    ```

=== "C++"

    `Server/Session/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Session/main.cpp:doc-sc-session-dispatch"
    ```

=== "Java"

    `Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java:doc-sc-session-dispatch"
    ```

=== "Kotlin"

    `Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-session-dispatch"
    ```

=== "Node/TypeScript"

    `Server/Session/Sessions/supportchat-session.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Session/Sessions/supportchat-session.ts:doc-sc-session-dispatch"
    ```

After authentication the session creates or finds the identity Actor and binds it to the current
session. For a customer that Actor is the conversation participant itself; for an agent it is the
roster Actor.

=== "C#/.NET"

    `Server/Session/Sessions/SupportChatSession.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-session-auth"
    ```

=== "C++"

    `Server/Session/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Session/main.cpp:doc-sc-session-auth"
    ```

=== "Java"

    `Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java:doc-sc-session-auth"
    ```

=== "Kotlin"

    `Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-session-auth"
    ```

=== "Node/TypeScript"

    `Server/Session/Sessions/supportchat-session.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Session/Sessions/supportchat-session.ts:doc-sc-session-auth"
    ```

Reconnecting with the same id binds a new session to the same Actor. Binding is covered by
[Session and Actor](24-actor-session.en.md).

## 5. Opening a Conversation and Assigning an Agent

The customer's open request is relayed to the identity Actor and arrives at the Entry Spot's actor
request handler. The handler asks API to create the conversation and reserves a join into the
conversation Spot with the id it receives.

=== "C#/.NET"

    `Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/OpenConversationActorHandler.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/OpenConversationActorHandler.cs:doc-sc-open-actor"
    ```

=== "C++"

    `Server/Support/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Support/main.cpp:doc-sc-open-actor"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/entryspot/handlers/OpenConversationActorHandler.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/entryspot/handlers/OpenConversationActorHandler.java:doc-sc-open-actor"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/OpenConversationActorHandler.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/OpenConversationActorHandler.kt:doc-sc-open-actor"
    ```

=== "Node/TypeScript"

    `Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-handlers.ts:doc-sc-open-actor"
    ```

API creates the conversation Spot through the Spot manager. The framework issues the id and picks the
owner.

=== "C#/.NET"

    `Server/Api/Handlers/OpenConversationHandler.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Api/Handlers/OpenConversationHandler.cs:doc-sc-api-open"
    ```

=== "C++"

    `Server/Api/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Api/main.cpp:doc-sc-api-open"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/infrastructure/FrameworkConversationSpotFactory.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/infrastructure/FrameworkConversationSpotFactory.java:doc-sc-api-open"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/FrameworkConversationStarter.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/FrameworkConversationStarter.kt:doc-sc-api-open"
    ```

=== "Node/TypeScript"

    `Server/Api/Handlers/open-conversation-handler.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Api/Handlers/open-conversation-handler.ts:doc-sc-api-open"
    ```

Once the customer Actor's join completes, the conversation Spot picks an agent with capacity left and
sends an assignment notify to that roster Actor's bound session. If no agent is left, the conversation
waits rather than failing.

=== "C#/.NET"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/ConversationSpot.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/ConversationSpot.cs:doc-sc-assign"
    ```

=== "C++"

    `Server/Support/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Support/main.cpp:doc-sc-assign"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/conversationspot/ConversationSpot.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/conversationspot/ConversationSpot.java:doc-sc-assign"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt:doc-sc-assign"
    ```

=== "Node/TypeScript"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts:doc-sc-assign"
    ```

The roster Actor lives in the Entry Spot and is not a member of the conversation Spot. Sending to its
bound session still needs no knowledge of where the Actor is.

=== "C#/.NET"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs:doc-sc-roster-push"
    ```

=== "C++"

    `Server/Support/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Support/main.cpp:doc-sc-roster-push"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/infrastructure/ConversationNotificationPublisher.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/infrastructure/ConversationNotificationPublisher.java:doc-sc-roster-push"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt:doc-sc-roster-push"
    ```

=== "Node/TypeScript"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts:doc-sc-roster-push"
    ```

Creating a User Spot is covered by [Spot](21-spot.en.md#4-the-calling-side--the-node-that-calls-a-spot),
and reserving a join by
[Actor Membership](35-actor-membership.en.md#2-reserving-a-join--it-runs-after-the-handler-ends).

## 6. The Agent Joining a Conversation — One Connection, Several Actors

An Actor belongs to one Spot at a time. The roster Actor has to stay in the Entry Spot, so when the
agent enters a conversation the session creates a new per-conversation Actor and binds it to the same
session as well.

=== "C#/.NET"

    `Server/Session/Sessions/SupportChatSession.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-agent-join"
    ```

=== "C++"

    `Server/Session/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Session/main.cpp:doc-sc-agent-join"
    ```

=== "Java"

    `Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java:doc-sc-agent-join"
    ```

=== "Kotlin"

    `Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-agent-join"
    ```

=== "Node/TypeScript"

    `Server/Session/Sessions/supportchat-session.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Session/Sessions/supportchat-session.ts:doc-sc-agent-join"
    ```

From then on the pushes of that conversation arrive at the conversation Actor's bound session — the
agent's same connection. How many may be bound and the rules are covered by
[How Session Binding Works](39-session-binding.en.md#1-how-many-may-be-bound--several-per-session-one-per-actor).

## 7. Chat — Actor-Based Relay

The agent finds a room's Actor handle using `JoinConversationRes.actorId` and sends chat packets
through that handle. The customer sends through the single Actor bound to its connection.
Authentication and `JoinConversationReq(conversationId)` are binding packets handled directly by the
Session handler. Session relays the other packets without decoding their payloads. If a packet has an
Actor slot, Session relays it to that Actor bound in the dispatch context; otherwise, it uses the
connection's identity Actor.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-chat-typing-en.html" title="Chat and typing" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-chat-typing-en.html" target="_blank">↗ View larger</a></p>

=== "C#/.NET"

    `Server/Session/Sessions/SupportChatSession.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-actor-relay"
    ```

=== "C++"

    `Server/Session/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Session/main.cpp:doc-sc-actor-relay"
    ```

=== "Java"

    `Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Session/src/main/java/systems/zlink/samples/supportchat/server/session/sessions/SupportChatSession.java:doc-sc-actor-relay"
    ```

=== "Kotlin"

    `Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-actor-relay"
    ```

=== "Node/TypeScript"

    `Server/Session/Sessions/supportchat-session.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Session/Sessions/supportchat-session.ts:doc-sc-actor-relay"
    ```

The conversation Spot assigns a `MessageSeq` to the message and pushes a notify to the bound session
of every participant except the sender.

=== "C#/.NET"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs:doc-sc-message-push"
    ```

=== "C++"

    `Server/Support/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Support/main.cpp:doc-sc-message-push"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/infrastructure/ConversationNotificationPublisher.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/infrastructure/ConversationNotificationPublisher.java:doc-sc-message-push"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt:doc-sc-message-push"
    ```

=== "Node/TypeScript"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts:doc-sc-message-push"
    ```

The lifetime of a reply token is covered by
[How STREAM Works](38-stream-boundary.en.md#3-the-lifetime-of-a-reply-token), and the one-at-a-time
execution boundary inside a Spot by [The Execution Model](32-execution-model.en.md).

## 8. The Idle Timer and Closing

Some time after the last message, the conversation Spot's timer moves the conversation to idle, and
if no message arrives within the grace period it closes it.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-idle-close-en.html" title="Idle, close and reconnect" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-idle-close-en.html" target="_blank">↗ View larger</a></p>

=== "C#/.NET"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/ConversationIdleTimerHandler.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/ConversationIdleTimerHandler.cs:doc-sc-idle-timer"
    ```

=== "C++"

    `Server/Support/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Support/main.cpp:doc-sc-idle-timer"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/conversationspot/ConversationIdleTimerHandler.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/conversationspot/ConversationIdleTimerHandler.java:doc-sc-idle-timer"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt:doc-sc-idle-timer"
    ```

=== "Node/TypeScript"

    `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/conversation-idle-timer-handler.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/conversation-idle-timer-handler.ts:doc-sc-idle-timer"
    ```

When a conversation closes, the assigned agent's capacity is returned. When the agent's connection
drops, the Entry Spot's disconnect callback withdraws the availability, and the conversation state
stays as it is and continues after reconnecting.

=== "C#/.NET"

    `Server/Support/Infrastructure/ZLink/Spots/EntrySpot/SupportEntrySpot.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/SupportEntrySpot.cs:doc-sc-agent-disconnect"
    ```

=== "C++"

    ```cpp
    // The C++ sample has no disconnect callback on its Entry Spot; availability is not withdrawn on disconnect.
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/entryspot/SupportEntrySpot.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/entryspot/SupportEntrySpot.java:doc-sc-agent-disconnect"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/SupportEntrySpot.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/SupportEntrySpot.kt:doc-sc-agent-disconnect"
    ```

=== "Node/TypeScript"

    `Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-spot.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-spot.ts:doc-sc-agent-disconnect"
    ```

The agent turns availability on and off with a request.

=== "C#/.NET"

    `Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/SetAgentAvailableHandler.cs`

    ```csharp
    --8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/SetAgentAvailableHandler.cs:doc-sc-set-available"
    ```

=== "C++"

    `Server/Support/main.cpp`

    ```cpp
    --8<-- "framework/languages/cpp/samples/SupportChat/Server/Support/main.cpp:doc-sc-set-available"
    ```

=== "Java"

    `Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/entryspot/handlers/SetAgentAvailableActorHandler.java`

    ```java
    --8<-- "framework/languages/java/samples/java/SupportChat/Server/Support/src/main/java/systems/zlink/samples/supportchat/server/support/spots/entryspot/handlers/SetAgentAvailableActorHandler.java:doc-sc-set-available"
    ```

=== "Kotlin"

    `Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/SetAgentAvailableHandler.kt`

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/SetAgentAvailableHandler.kt:doc-sc-set-available"
    ```

=== "Node/TypeScript"

    `Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-handlers.ts`

    ```typescript
    --8<-- "framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-handlers.ts:doc-sc-set-available"
    ```

Timers are covered by [Timers and Workers](36-timer-worker.en.md#1-timers--periodic-execution), and
where the disconnect notification lands by
[How Session Binding Works](39-session-binding.en.md#3-notification-when-the-connection-drops).

## 9. Running and Verifying

One runner starts the Redis container, the server processes and the client scenario together.

=== "C#/.NET"

    ```bash
    framework/languages/dotnet/samples/SupportChat/run_sample.sh
    ```

=== "C++"

    ```bash
    framework/languages/cpp/samples/SupportChat/run_sample.sh
    ```

=== "Java"

    ```bash
    framework/languages/java/samples/java/SupportChat/run_sample.sh
    ```

=== "Kotlin"

    ```bash
    framework/languages/java/samples/kotlin/SupportChat/run_sample.sh
    ```

=== "Node/TypeScript"

    ```bash
    framework/languages/node/samples/SupportChat.Ts/run_sample.sh
    ```

The client asserts the assignment notify, the `MessageSeq` order, the typing notify, the idle and close
transitions and the state after reconnecting. The checks and the exact log strings are set by the
[SupportChat scenario](../../../common/sample/supportchat/README.en.md#9-client-self-check).

## 10. Related Documents

- Comparison with the other samples and how to choose: [Picking a Sample](14-samples.en.md)
- Requirements, message contract and completion criteria:
  [SupportChat scenario](../../../common/sample/supportchat/README.en.md)
- A layout that binds a single Actor per session: [Reading Along: Bingo](50-bingo.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
