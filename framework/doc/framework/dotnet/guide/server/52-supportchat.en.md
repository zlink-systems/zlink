---
title: "Reading Along: SupportChat · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/52-supportchat.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Reading Along: SupportChat

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Reading Along: TicTacToe](51-tictactoe.en.md) | [Next: Reading Along: DeliveryDispatch](53-deliverydispatch.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/52-supportchat.en.md) · **C#/.NET** · [Java](../../../java/guide/server/52-supportchat.en.md) · [Kotlin](../../../kotlin/guide/server/52-supportchat.en.md) · [Node/TypeScript](../../../node/guide/server/52-supportchat.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can open the SupportChat sample in an editor and follow a message from the customer's
    request to open a conversation, through agent assignment, to the conversation closing, through
    the code of each server it passes. The code in this chapter runs as it stands in
    `framework/languages/<language>/samples/SupportChat`.

[Picking a Sample](14-samples.en.md#4-supportchat--building-a-live-chat-support-system) introduced
what this sample demonstrates. This chapter is what you read after that introduction — the roles and
where their code lives, the message flow of the main scenarios, and, for each flow, the framework
feature it uses and the chapter that explains it, in the order the source is laid out. This chapter
has no spec document that owns a contract. The requirements, message contract and verification
criteria are owned by the [SupportChat scenario](../../../common/sample/supportchat/README.en.md),
and this chapter does not restate them.

## 1. What This Sample Demonstrates

The customer and the agent each keep a single STREAM connection to the Session server. API handles
authentication, and the conversation state is owned by a conversation Spot on Support. One agent
handles several conversations at once, so the agent's single connection has a roster Actor and one
conversation Actor per conversation bound to it, and an incoming packet is routed to the right Actor
by the `ConversationId` in the stream metadata.

<iframe class="zlink-diagram" src="/common/diagrams/14-supportchat-en.html" title="SupportChat sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-supportchat-en.html" target="_blank">↗ View larger</a></p>

The flow this chapter follows is authentication and the identity Actor binding → opening a
conversation and assignment → the agent joining the conversation → chat relayed by metadata → the idle
timer and closing.

## 2. Roles and Where the Code Lives

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| Session | 1 | STREAM connections, authentication packets, Actor binding and metadata relay | `Server/Session` |
| API | 1 | Token verification, the request to create a conversation Spot | `Server/Api` |
| Support | 1 | Entry Spot, conversation Spot, identity, roster and conversation Actors, assignment and pushes | `Server/Support/Infrastructure` |
| Client | 1 | The customer and agent scenarios, with self-checks | `Client` |

Participants, `MessageSeq`, typing and the idle/close transitions live in `Server/Support/Domain` and
reference no framework type. The agent capacity decision belongs to `Server/Support/Application`.
Messages are set by the JSON contract in `Shared`.

## 3. Server Configuration

Session calls objects in the Support mesh, calls the API channel, and receives STREAM connections.

`Server/Session/SessionServerHostFactory.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/SessionServerHostFactory.cs:doc-sc-session-register"
```

Support registers the Entry Spot, the Actor factory and the conversation Spot factory, and allows the
metadata key that may travel between a session and an Actor.

`Server/Support/SupportServerHostFactory.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/SupportServerHostFactory.cs:doc-sc-support-register"
```

Without that allowance, the `ConversationId` the session attaches is silently dropped rather than
rejected. The allowed key is listed among the values that must be set in
[Options](16-options.en.md#10-values-that-must-be-set).

## 4. Authentication and the Identity Actor

The first packet the client sends is an authentication request. The session splits packets by name
into authentication, conversation join and everything else.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-auth-join-en.html" title="Authentication, conversation creation and agent join" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-auth-join-en.html" target="_blank">↗ View larger</a></p>

`Server/Session/Sessions/SupportChatSession.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-session-dispatch"
```

After authentication the session creates or finds the identity Actor and binds it to the current
session. For a customer that Actor is the conversation participant itself; for an agent it is the
roster Actor.

`Server/Session/Sessions/SupportChatSession.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-session-auth"
```

Reconnecting with the same id binds a new session to the same Actor. Binding is covered by
[Session and Actor](24-actor-session.en.md).

## 5. Opening a Conversation and Assigning an Agent

The customer's open request is relayed to the identity Actor and arrives at the Entry Spot's actor
request handler. The handler asks API to create the conversation and reserves a join into the
conversation Spot with the id it receives.

`Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/OpenConversationActorHandler.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/OpenConversationActorHandler.cs:doc-sc-open-actor"
```

API creates the conversation Spot through the Spot manager. The framework issues the id and picks the
owner.

`Server/Api/Handlers/OpenConversationHandler.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Api/Handlers/OpenConversationHandler.cs:doc-sc-api-open"
```

Once the customer Actor's join completes, the conversation Spot picks an agent with capacity left and
sends an assignment notify to that roster Actor's bound session. If no agent is left, the conversation
waits rather than failing.

`Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/ConversationSpot.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/ConversationSpot.cs:doc-sc-assign"
```

The roster Actor lives in the Entry Spot and is not a member of the conversation Spot. Sending to its
bound session still needs no knowledge of where the Actor is.

`Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs:doc-sc-roster-push"
```

Creating a User Spot is covered by [Spot](21-spot.en.md#4-the-calling-side--the-node-that-calls-a-spot),
and reserving a join by
[Actor Membership](35-actor-membership.en.md#2-reserving-a-join--it-runs-after-the-handler-ends).

## 6. The Agent Joining a Conversation — One Connection, Several Actors

An Actor belongs to one Spot at a time. The roster Actor has to stay in the Entry Spot, so when the
agent enters a conversation the session creates a new per-conversation Actor and binds it to the same
session as well.

`Server/Session/Sessions/SupportChatSession.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-agent-join"
```

From then on the pushes of that conversation arrive at the conversation Actor's bound session — the
agent's same connection. How many may be bound and the rules are covered by
[How Session Binding Works](39-session-binding.en.md#1-how-many-may-be-bound--several-per-session-one-per-actor).

## 7. Chat — Relay Chosen by Metadata

A chat packet is not decoded; the target Actor is chosen by the `ConversationId` in the stream
metadata. If the agent's map has it, the packet is relayed to that conversation Actor, otherwise to
the identity Actor.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-chat-typing-en.html" title="Chat and typing" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-chat-typing-en.html" target="_blank">↗ View larger</a></p>

`Server/Session/Sessions/SupportChatSession.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Session/Sessions/SupportChatSession.cs:doc-sc-metadata-relay"
```

The conversation Spot assigns a `MessageSeq` to the message and pushes a notify to the bound session
of every participant except the sender.

`Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/ConversationNotificationPublisher.cs:doc-sc-message-push"
```

The lifetime of a reply token is covered by
[How STREAM Works](38-stream-boundary.en.md#3-the-lifetime-of-a-reply-token), and the one-at-a-time
execution boundary inside a Spot by [The Execution Model](32-execution-model.en.md).

## 8. The Idle Timer and Closing

Some time after the last message, the conversation Spot's timer moves the conversation to idle, and
if no message arrives within the grace period it closes it.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-idle-close-en.html" title="Idle, close and reconnect" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-idle-close-en.html" target="_blank">↗ View larger</a></p>

`Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/ConversationIdleTimerHandler.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/ConversationIdleTimerHandler.cs:doc-sc-idle-timer"
```

When a conversation closes, the assigned agent's capacity is returned. When the agent's connection
drops, the Entry Spot's disconnect callback withdraws the availability, and the conversation state
stays as it is and continues after reconnecting.

`Server/Support/Infrastructure/ZLink/Spots/EntrySpot/SupportEntrySpot.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/SupportEntrySpot.cs:doc-sc-agent-disconnect"
```

The agent turns availability on and off with a request.

`Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/SetAgentAvailableHandler.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/SupportChat/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/SetAgentAvailableHandler.cs:doc-sc-set-available"
```

Timers are covered by [Timers and Workers](36-timer-worker.en.md#1-timers--periodic-execution), and
where the disconnect notification lands by
[How Session Binding Works](39-session-binding.en.md#3-notification-when-the-connection-drops).

## 9. Running and Verifying

One runner starts the Redis container, the server processes and the client scenario together.

```bash
framework/languages/dotnet/samples/SupportChat/run_sample.sh
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
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
