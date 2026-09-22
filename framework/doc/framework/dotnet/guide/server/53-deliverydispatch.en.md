---
title: "Reading Along: DeliveryDispatch · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/53-deliverydispatch.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Reading Along: DeliveryDispatch

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Reading Along: SupportChat](52-supportchat.en.md) | [Next: Reading Along: ShoppingMall](54-shoppingmall.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/53-deliverydispatch.en.md) · **C#/.NET** · [Java](../../../java/guide/server/53-deliverydispatch.en.md) · [Kotlin](../../../kotlin/guide/server/53-deliverydispatch.en.md) · [Node/TypeScript](../../../node/guide/server/53-deliverydispatch.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can open the DeliveryDispatch sample in an editor and follow the code from a delivery
    request accepted over HTTP, through the offer to a courier and the reassignment on no answer, to
    the status pushed to the customer. The code in this chapter runs as it stands in
    `framework/languages/dotnet/samples/DeliveryDispatch`.

[Picking a Sample](14-samples.en.md#6-deliverydispatch--building-a-dispatch-system) introduced what
this sample demonstrates. This chapter is what you read after that introduction — the roles and where
their code lives, the message flow of the main scenarios, and, for each flow, the framework feature it
uses and the chapter that explains it, in the order the source is laid out. This chapter has no spec
document that owns a contract. The requirements, message contract and verification criteria are owned
by the [DeliveryDispatch scenario](../../../common/sample/deliverydispatch/README.en.md), and this
chapter does not restate them.

## 1. What This Sample Demonstrates

The customer creates a delivery over HTTP and receives its status over STREAM. The courier receives
offers over STREAM and sends decisions. The servers in between keep no session map and no socket
registry — the courier and the customer are each an Actor with an id, and offers and statuses are
pushed to the session bound to that Actor. No handler holds an execution turn while waiting for the
courier's answer; the offer deadline is a record that Dispatch manages.

<iframe class="zlink-diagram" src="/common/diagrams/14-delivery-en.html" title="DeliveryDispatch sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-delivery-en.html" target="_blank">↗ View larger</a></p>

The flow this chapter follows is the courier binding → HTTP intake and the first offer → the offer
push and the decision → reassignment on timeout → status recording and the customer push.

## 2. Roles and Where the Code Lives

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| Dispatch | 1 | HTTP intake, candidate selection, offer records and deadlines, reassignment | `Server/Dispatch` |
| CourierSession | 1 | Courier STREAM, courier Actor binding and relay | `Server/CourierSession` |
| CourierActorNode | 2 | Courier Entry Spot and courier Actors | `Server/CourierActorNode` |
| Tracking | 1 | Status event recording, forwarding to the customer Actor | `Server/Tracking` |
| CustomerGateway | 1 | Customer STREAM, customer Actor binding and pushes | `Server/CustomerGateway` |
| Client | 1 | The customer and courier scenarios with self-checks | `Client` |

Candidate order, deadlines and attempt validation belong to Dispatch's Application, and the status
transitions to Tracking's Domain; neither references a framework type. Messages are set by the JSON
contract in `Shared`.

## 3. Server Configuration

Dispatch is an object client of the courier mesh, the server of the dispatch channel, and a client of
the tracking channel. In the implementations whose HTTP handler sends the request over the dispatch
channel, the same process is that channel's client as well.

`Server/Dispatch/DispatchServerHostFactory.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchServerHostFactory.cs:doc-dd-dispatch-register"
```

CourierActorNode registers the courier Entry Spot and the courier Actor factory as an object server.
Both nodes provide the same type, so the framework picks the node an Actor is created on.

`Server/CourierActorNode/NodeHostFactory.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/CourierActorNode/NodeHostFactory.cs:doc-dd-node-register"
```

The client and server roles of a channel are covered by
[Channel Messaging](20-channel-messaging.en.md#4-clientserver-channel), and the difference between an
object server and an object client by
[Spot](21-spot.en.md#3-the-receiving-side--the-node-that-hosts-the-spot).

## 4. Courier Binding

When the courier client connects over STREAM and asks to bind, CourierSession creates or finds the
Actor by the courier id and binds it to the current session.

`Server/CourierSession/CourierSessionBinder.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/CourierSession/CourierSessionBinder.cs:doc-dd-session-bind"
```

The call that obtains the Actor does not decide which CourierActorNode creates it. Binding is covered
by [Session and Actor](24-actor-session.en.md).

## 5. HTTP Intake and the First Offer

The customer's HTTP request arrives at Dispatch's endpoint. The handler hands the request to the
worker and replies at once — some implementations do so with a one-way send on the dispatch
channel, others through an in-process queue.

<iframe class="zlink-diagram" src="/common/diagrams/sample-delivery-success-en.html" title="Normal flow — dispatch, acceptance and status pushes" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-delivery-success-en.html" target="_blank">↗ View larger</a></p>

`Server/Dispatch/DispatchServerHostFactory.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchServerHostFactory.cs:doc-dd-http-create"
```

The worker picks the first candidate, reports the Assigned status to Tracking, records the offer with
its attempt and deadline, and sends the offer to the courier Actor. The turn that sends the offer
ends there.

`Server/Dispatch/DispatchWorker.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchWorker.cs:doc-dd-offer-start"
```

The offer is an Actor direct send addressed by the courier id. It is not a request, so nothing waits
for a reply.

`Server/Dispatch/DispatchZLinkAdapters.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchZLinkAdapters.cs:doc-dd-offer-send"
```

Calling an Actor by id is covered by
[Actor](22-actor.en.md#3-the-calling-side--the-node-that-calls-an-actor), and the difference between
send and request by [Channel Messaging](20-channel-messaging.en.md#1-request-and-response).

## 6. The Offer Push and the Decision

The offer arrives at the courier Actor's Entry Spot handler. The Actor remembers the attempt and
pushes an offer notify to its bound session.

`Server/CourierActorNode/CourierActor.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/CourierActorNode/CourierActor.cs:doc-dd-offer-push"
```

The courier's decision comes in over the same connection and is relayed to the Actor. The handler
attaches the remembered attempt to the decision and sends it one-way to the dispatch channel. The
node neither judges the decision nor times it.

`Server/CourierActorNode/Spots/EntrySpot/Handlers/CourierDecisionActorHandler.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/CourierActorNode/Spots/EntrySpot/Handlers/CourierDecisionActorHandler.cs:doc-dd-decision-send"
```

Dispatch's handler applies a decision only when its attempt equals the current attempt on record. A
decision from the previous courier that arrives after a reassignment is ignored as stale.

`Server/Dispatch/OfferDeliveryResultHandler.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/OfferDeliveryResultHandler.cs:doc-dd-decision-settle"
```

The handler kinds are covered by
[Handlers and Message Processing](31-handler-dispatch.en.md#4-the-handler-kinds-of-spots-and-actors).

## 7. Reassignment on Timeout

The deadline is checked by Dispatch's sweeper. Each period it takes the expired offers and calls
reassignment; no handler waits for a courier.

<iframe class="zlink-diagram" src="/common/diagrams/sample-delivery-reassign-en.html" title="Reassignment on timeout — promotion to attempt 2" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-delivery-reassign-en.html" target="_blank">↗ View larger</a></p>

`Server/Dispatch/DispatchWorker.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchWorker.cs:doc-dd-sweeper"
```

Reassignment repeats the same procedure with the next candidate under a new attempt. When no candidate
is left, the delivery ends in the Failed status.

`Server/Dispatch/DispatchWorker.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchWorker.cs:doc-dd-reassign"
```

The sweeper is a periodic job of the host, not a framework timer — the deadline is not the state of
any Spot. The form to use when periodic execution inside a Spot is needed is covered by
[Timers and Workers](36-timer-worker.en.md#1-timers--periodic-execution).

## 8. Status Recording and the Customer Push

A status change reaches Tracking as a request on the tracking channel. Tracking records the evidence
and forwards it to CustomerGateway with an Actor direct send addressed by the customer id.

`Server/Tracking/Handlers.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/Tracking/Handlers.cs:doc-dd-tracking-forward"
```

The customer Actor's Entry Spot handler receives it and pushes a status notify to the bound session.
When the customer reconnects, a new session is bound to the same Actor, so the push goes to the new
connection.

`Server/CustomerGateway/Spots/EntrySpot/Handlers/DeliveryStatusUpdatedHandler.cs`

```csharp
--8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Server/CustomerGateway/Spots/EntrySpot/Handlers/DeliveryStatusUpdatedHandler.cs:doc-dd-customer-push"
```

The path to a bound session and the refresh after reconnecting are covered by
[How Session Binding Works](39-session-binding.en.md).

## 9. Running and Verifying

One runner starts the Redis container, the server processes and the client scenario together.

```bash
framework/languages/dotnet/samples/DeliveryDispatch/run_sample.sh
```

The client asserts the order of the status notifies and their `DeliveryId` in both the normal
dispatch flow and the timeout reassignment flow. The checks and the exact log strings are set by the
[DeliveryDispatch scenario](../../../common/sample/deliverydispatch/README.en.md#9-client-self-check).

## 10. Related Documents

- Comparison with the other samples and how to choose: [Picking a Sample](14-samples.en.md)
- Requirements, message contract and completion criteria:
  [DeliveryDispatch scenario](../../../common/sample/deliverydispatch/README.en.md)
- A layout that binds several Actors to one session: [Reading Along: SupportChat](52-supportchat.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
