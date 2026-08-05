# Event Sample Scenarios

[Sample List](../README.ko.md)

This directory defines common sample scenarios that showcase framework features centered on event
propagation. Both samples deal with events, but with different purposes.

The documents in this directory follow the same writing standard as the parent sample spec. Like the
`.NET` Bingo and TicTacToe samples, server roles, connection method, message contract, flow, client
scenarios, and implementation-completion criteria must all be confirmable within one document. The
only difference is the event's authoritative path. A business flow that needs durable events first
builds the required meaning with a domain event store, state owner, and projection. As the event
volume and consumer count grow, Redis Stream or Kafka is kept as an expansion path. A realtime flow
that can tolerate loss and be corrected via a snapshot uses whichever ZLink messaging path — fanout
or owner routing — fits the domain's ownership boundary. When event sourcing makes the domain model
clearer, the event stream is treated as the authoritative state, and the projection as a rebuildable
read model.

| Sample | Purpose | Event Authoritative Path | ZLink Role |
|------|------|----------------|------------|
| [ShoppingMall](shoppingmall.en.md) | Separates `CommerceApi` (the HTTP edge) from `OrderWorkflow` (the order owner) to build a robust event-sourced order workflow. | ZLink owner routing + OrderEventStore | event sourcing, workflow owner spot, projection lookup |
| [GameQuest](gamequest.en.md) | Gathers gameplay events into a per-player owner spot to update an event-sourced quest aggregate. | ZLink owner routing + QuestEventStore | owner spot serialization, event sourcing, WebSocket notify |

ShoppingMall doesn't just clone Kafka. Even a commerce service starting at a small scale must handle
order, inventory, and payment workflows robustly against failures and duplicate requests. This sample
separates the `CommerceApi` server that terminates HTTP from the `OrderWorkflow` server that owns
orders, composing order state transitions as an event-sourced workflow with
`OrderWorkflowSpot`/`OrderEventStore`/projection. Redis Stream or Kafka is an expansion option
attached when you need many consumers, a large backlog, or external downstream replay.

GameQuest is a sample that showcases owner-routed gameplay events together with event sourcing. The
`Session Server` handles combat, inventory, mission, and world action, sending gameplay events to an
owner spot keyed by `PlayerId`. `PlayerQuestSpot` replays the quest event stream to restore the
aggregate, then appends a new quest domain event. Client lookups and notifications use the
projection, and any possible loss is corrected by re-appending a snapshot resynchronization result
to the event stream.
