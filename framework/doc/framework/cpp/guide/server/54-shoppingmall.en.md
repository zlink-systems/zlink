---
title: "Reading Along: ShoppingMall · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/54-shoppingmall.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Reading Along: ShoppingMall

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Reading Along: DeliveryDispatch](53-deliverydispatch.en.md) | [Next: Reading Along: GameQuest](55-gamequest.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/server/54-shoppingmall.en.md) · [Java](../../../java/guide/server/54-shoppingmall.en.md) · [Kotlin](../../../kotlin/guide/server/54-shoppingmall.en.md) · [Node/TypeScript](../../../node/guide/server/54-shoppingmall.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can open the ShoppingMall sample in an editor and follow the code from an order accepted over
    HTTP, through inventory reservation → payment authorization → confirmation inside the owner Spot,
    and through compensation when a step fails. The code in this chapter comes from the [ShoppingMall sample in the per-language example repositories](https://github.com/zlink-systems/zlink-cpp-examples/tree/main/samples/ShoppingMall).

[Picking a Sample](14-samples.en.md#6-shoppingmall--building-an-order-processing-system) introduced
what this sample demonstrates. This chapter is what you read after that introduction — the roles and
where their code lives, the message flow of the main scenarios, and, for each flow, the framework
feature it uses and the chapter that explains it, in the order the source is laid out. This chapter
explains the ShoppingMall sample's roles and code locations, its main message flows, and its run
verification in source order. See the [ShoppingMall scenario](../../../common/sample/event/shoppingmall.en.md)
for requirements, message contracts, and verification criteria.

## 1. What This Sample Demonstrates

One order is owned by an Instance Spot whose id is the `OrderId`. The API terminates HTTP, never
changes order state itself, and only sends requests to that id. The code inside the Spot re-reads the
event stream to learn the current step, executes exactly one next step, and records an event — there
is no separate coordination state, scheduler or outbox layer.

<iframe class="zlink-diagram" src="/common/diagrams/14-shoppingmall-en.html" title="ShoppingMall sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-shoppingmall-en.html" target="_blank">↗ View larger</a></p>

Event sourcing is not a framework feature; it is an application design this sample builds on top of
a Spot. What the framework provides is routing to the current owner by `OrderId`, creation on the
first request, and executing one order's handlers one at a time.

## 2. Roles and Where the Code Lives

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| CommerceApi | 2 | HTTP input validation, idempotency mapping, workflow requests, read model queries | `Server/CommerceApi` |
| OrderWorkflow | 2 | The order workflow Instance Spot, replay, next step and event recording, external module adapters | `Server/OrderWorkflow` |
| Client | 1 | Order start, polling, and the duplicate, failure, resume and rebuild assertions | `Client` |

State transitions, event creation and compensation rules live in `Server/OrderWorkflow/Domain` and
reference no framework type. Replay, fold, the next-step decision and expected-version recording
belong to `Server/OrderWorkflow/Application`. Messages are set by the JSON contract in `Shared`.

## 3. Server Configuration

OrderWorkflow registers the order workflow Instance Spot factory on the mesh's object server. The
factory chooses the policy that recreates the Spot on the target during relocation instead of moving
state — the state can be restored from the event stream.

`Server/OrderWorkflow/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/OrderWorkflow/main.cpp:doc-sm-workflow-register"
```

CommerceApi is an object client of the same mesh. There are two APIs and two Workflows, and neither
side knows the other's endpoint.

`Server/CommerceApi/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/CommerceApi/main.cpp:doc-sm-api-register"
```

The creation policy of an Instance Spot is covered by
[Activation and Lifetime](34-activation-lifetime.en.md#1-the-kinds-of-spot), and the recreate-on-move
policy by [Relocation](37-relocation.en.md#3-when-state-is-captured--the-factory-registration-decides).

## 4. Starting an Order — the First Request Creates the Owner

When the client starts an order over HTTP, the API checks the idempotency key. If the order has
already started it returns the read model; otherwise it reserves the mapping and asks the workflow to
Start.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-start-success-en.html" title="Order start and the success flow" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-start-success-en.html" target="_blank">↗ View larger</a></p>

`Server/CommerceApi/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/CommerceApi/main.cpp:doc-sm-api-start"
```

Every request to the workflow has the same shape — the `OrderId` as the Spot id, together with the
Instance Spot type and the mesh. If no Spot has that id, the first request creates one on one of the
eligible nodes; if one exists, the current owner receives the request.

`Server/CommerceApi/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/CommerceApi/main.cpp:doc-sm-api-request"
```

On the Spot side the request handler passes the request to the Spot's start method.

`Server/OrderWorkflow/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/OrderWorkflow/main.cpp:doc-sm-start-handler"
```

Start records the started event in the event stream and replies with the `Created` state.
Reservation, authorization and confirmation follow after the reply.

`Server/OrderWorkflow/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/OrderWorkflow/main.cpp:doc-sm-spot-start"
```

Calling an Instance Spot is covered by [Spot](21-spot.en.md#6-the-other-kinds-of-spot), and the rule
that concurrent requests for the same id wait for a single creation by
[Activation and Lifetime](34-activation-lifetime.en.md).

## 5. Advancing the Steps — Replay, Next Step, Record

The HTTP reply ends at `Created`; the remaining steps are driven by a background continuation. The
client polls until it sees `Confirmed` or `Failed`.

`Server/OrderWorkflow/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/OrderWorkflow/main.cpp:doc-sm-background-continue"
```

One turn of the continuation starts by reading the event stream and restoring the aggregate. No
progress pointer is stored separately — the last event is the current step.

`Server/Common/workflow_logic.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/Common/workflow_logic.hpp:doc-sm-replay"
```

Depending on the current step, exactly one next step runs. If the inventory reservation is refused,
payment is not called; if payment is refused, the reservation already recorded is released and the
order ends as failed.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-failure-compensation-en.html" title="Inventory failure and payment-failure compensation" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-failure-compensation-en.html" target="_blank">↗ View larger</a></p>

`Server/Common/workflow_logic.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/Common/workflow_logic.hpp:doc-sm-next-step"
```

Events are recorded with an expected version. If a previous owner is still around and tries to write
to the same stream, the version mismatches and the write fails, so two owners never execute the same
step twice. After recording, the read model is updated.

`Server/Common/workflow_logic.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/Common/workflow_logic.hpp:doc-sm-append"
```

All of these steps run inside the same Spot's turn, so one order has no competing writers. The
serialization boundary is covered by [The Execution Model](32-execution-model.en.md).

## 6. Duplicates, Resume and Projection Rebuild

When the same key starts twice, both requests use the `OrderId` of the one that won the mapping
reservation, and a command already in the stream is not recorded again. An interrupted order is
resumed by a resume command that runs the same continuation again — replay reports the last step, so
completed steps are skipped.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-duplicate-resume-en.html" title="Duplicate start and resume after interruption" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-duplicate-resume-en.html" target="_blank">↗ View larger</a></p>

A query reads only the read model and does not advance the order.

`Server/CommerceApi/main.cpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/CommerceApi/main.cpp:doc-sm-get-state"
```

When the read model is deleted or inconsistent, it is rebuilt from the event stream alone.

`Server/Common/workflow_logic.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/ShoppingMall/Server/Common/workflow_logic.hpp:doc-sm-rebuild"
```

## 7. Termination and Lifecycle

Once the order reaches `Confirmed` or `Failed`, the Spot may close itself — the .NET and Node
implementations do. A request with the same `OrderId` after the close creates a Spot of a new
generation, which replays the stream and returns the terminal state as it is.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-lifecycle-en.html" title="Lifecycle and the failure boundary" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-lifecycle-en.html" target="_blank">↗ View larger</a></p>

```cpp
// This implementation's order workflow Spot does not Close on a terminal state.
```

If the Ready owner's process disappears, the request in flight ends as Unavailable, and the framework
does not create the same order on another node by itself. Planned relocation is a separate procedure,
and the resume after it continues through the same replay shown above. Close and generations are
covered by [Activation and Lifetime](34-activation-lifetime.en.md), and planned moves by
[Relocation](37-relocation.en.md).

## 8. Running and Verifying

One runner starts the Redis container, the server processes and the client scenario together.

```bash
framework/languages/cpp/samples/ShoppingMall/run_sample.sh
```

The client asserts the success and failure results, the duplicate start, the resume and the
projection rebuild. The checks and the exact log strings are set by the
[ShoppingMall scenario](../../../common/sample/event/shoppingmall.en.md#9-client-self-check).

## 9. Related Documents

- Comparison with the other samples and how to choose: [Picking a Sample](14-samples.en.md)
- Requirements, message contract and completion criteria:
  [ShoppingMall scenario](../../../common/sample/event/shoppingmall.en.md)
- The same owner Spot and event sourcing applied to a domain that tolerates loss:
  [Reading Along: GameQuest](55-gamequest.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
