<!-- framework-adapter-nav:start -->
[E2E Index](README.en.md) | [Previous: Spot Service](config-2-spot-service.en.md) | [Next: Registration And Codec](config-4-registration-codec.en.md)
<!-- framework-adapter-nav:end -->

# Config 3 — Classic Fanout Publish And Subscribers

Classic fanout delivers an event sent by one publisher to every subscriber that is currently ready.
Publish completion is not subscriber-receipt confirmation, and events are not replayed for a
subscriber that connected late or was disconnected while the event went out. In automatic mode, a
subscriber does not take an endpoint as input — it finds the publisher of the same ChannelName
through the Location Store.

This config verifies delivery, reconnect, discovery, and liveness using only the real publisher and
subscriber processes and the public fanout API. It does not use raw PUB/SUB frames, private
descriptors, or socket monitors.

## 1. Verification Scope

- Fanout delivery to multiple ready subscribers and packet-name handler selection
- Non-replay for late subscribers, subscriber reconnect, and publisher restart
- Automatic publisher discovery, Store failure/lease, and port changes
- Manual endpoint mode and startup validation
- Per-publisher liveness, reserved-topic validation, and orderly disconnect
- Isolation of a slow subscriber and a slow status observer

## 2. Deployment Configuration

| Role | Count | Purpose and reason for separation |
|---|---:|---|
| Location Store | 1 | Provides automatic publisher descriptors and owner leases. Uses a dedicated namespace per run. |
| Publisher | 1–2 per scenario | Publishes typed events on the `events` fanout Channel. Automatic mode uses port 0 and a Publisher RID; manual mode uses a separate fixed endpoint. |
| Subscriber | 2–3 per scenario | Provides a typed handler per packet name and a public fanout status endpoint. Uses either automatic or manual mode, not both. |
| E2E client | 1 | Publishes through the publisher's application endpoint and queries the subscriber's public evidence. |

The subscriber handler records the publisher marker, packet name, sequence, and typed payload in
application state. Fanout status and observer return values are also checked from the role server's
public evidence endpoint. The transport endpoint is included in the Application configuration only
in manual mode.

## 3. Common Run And Judgment Method

The runner creates new processes, a Store namespace, and a sequence range for each scenario. After
confirming from the automatic subscriber's public status that the publisher is `Ready`, it sends the
measurement event. Because the status observer may coalesce intermediate states, the runner does not
require every transition — it cross-checks the final `GetStatus` against the actual events received.

Publish terminal and remote delivery are judged separately. Publish is confirmed by the local
admission result, and delivery is confirmed by subscriber handler evidence. Classic fanout provides
no replay, so an event that arrives before connection or that shows up later for a period of
disconnection is a failure.

## 4. Scenarios

### Track A — Deliver Events To Ready Subscribers

#### PS-A1 Ready Subscribers Receive The Same Event

Priority: `P0`

When a publisher fans an event out to multiple subscribers, each ready subscriber must be able to
receive it. Classic fanout does not guarantee identical ordering or lossless delivery across
subscribers, so this scenario does not judge cross-subscriber ordering.

**Verification question:** Do three ready subscribers each observe the marker of a published event?

- Starting condition: The public status of all three subscribers shows the same publisher as ready,
  and each handler gate is open. A single small marker is used, with no network blocking or
  intentional backpressure introduced.
- Procedure: The publisher publishes a `fanout-ready` marker dedicated to this scenario. Each
  subscriber's application evidence is confirmed with a bounded wait.
- Verification: All three subscribers each record the `fanout-ready` marker. Delivery is not judged
  by publish terminal alone, and neither receive order across subscribers nor a complete unbroken
  sequence is required.
- Detailed behavior: verifies fanout delivery in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

#### PS-A2 Packet Name Selects The Typed Handler

Priority: `P0`

Even when multiple event types go out on the same fanout Channel, only the handler registered for
the packet name must run.

**Verification question:** Do events of two packet names each get processed exactly once, only by
their corresponding typed handler?

- Starting condition: The subscriber registers `InventoryChangedNotify` and `PriceChangedNotify`
  handlers, and the publisher is ready.
- Procedure: The two events are each published once with distinct markers.
- Verification: Each marker is recorded exactly once, only in the corresponding handler's evidence,
  and the typed payload value matches the input. The handler is not reselected by topic or payload
  field.
- Detailed behavior: verifies the handler namespace in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

#### PS-A3 A Late Subscriber Receives Events Starting From Ready

Priority: `P0`

Classic fanout does not retain past events. A subscriber that starts late must not receive events
published before it connected, and must receive only new events from after it becomes ready.

**Verification question:** Does a late subscriber receive only the event published after it becomes
ready, without replaying the event published before?

- Starting condition: The publisher and an existing subscriber are ready; the late subscriber's
  process has not started.
- Procedure: `before-ready` is published, then the late subscriber is started. Once its public status
  becomes ready, `after-ready` is published.
- Verification: The late subscriber receives `after-ready` exactly once and does not receive
  `before-ready`. The existing subscriber receives both events.
- Detailed behavior: verifies non-replay delivery in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

#### PS-A4 A Subscriber Receives New Events After Reconnect

Priority: `P1`

Even if the transport disconnects and recovers, the Application does not need to re-register its
handler. Events during the disconnection, however, are not replayed.

**Verification question:** Does the subscriber receive events after reconnect using its existing
handler registration, and not receive events published during the disconnection?

- Starting condition: Subscribers A and B both see the same publisher as ready and have received a
  baseline event.
- Procedure: The runner blocks only A's receive network from the publisher. Once A's public status
  becomes not-ready, `while-disconnected` is published and B's receipt is confirmed. The block is
  released; once A becomes ready, `after-reconnect` is published.
- Verification: B receives both events, and A receives only `after-reconnect`. A's process and
  handler registration persist throughout.
- Detailed behavior: verifies fanout reconnect in [Transport Liveness §6](../spec/29-transport-liveness.en.md#6-connection-loss-and-reconnect).

### Track B — Isolate Processing Across Subscribers

#### PS-B1 A Slow Subscriber Does Not Block Another Subscriber

Priority: `P1`

Each subscriber processes events in its own separate process. Even if one handler takes a long time,
another subscriber's handler must keep running.

**Verification question:** While subscriber A's handler is waiting, does subscriber B process a
subsequent event?

- Starting condition: A and B are ready, and A's handler is configured to wait on an application
  signal at the first marker.
- Procedure: A gate marker is published to confirm A has entered the handler, and a `B-follow-up`
  marker is published. B's evidence is confirmed, then A's gate is released.
- Verification: While A is waiting, B processes the `B-follow-up` marker. This scenario does not set
  a pass condition on B's receive order or how many events A catches up on or drops.
- Detailed behavior: verifies subscriber process and handler dispatch isolation in [Channel Topology](../spec/07-channel-topology.en.md).

#### PS-B2 An Existing Subscriber Receives New Events After Publisher Restart

Priority: `P1`

When the publisher restarts, the subscriber must build a new connection and become ready after the
first normal record. It does not re-register the Application handler or replay past events.

**Verification question:** After publisher restart, does the existing subscriber receive new events
without replaying events from the interruption period?

- Starting condition: The publisher and subscriber are ready and have received a baseline event.
- Procedure: The publisher shuts down normally, and the subscriber's status becomes not-ready. A
  publisher of the same role is restarted; once it becomes ready, a new marker is published.
- Verification: The existing subscriber process receives the new marker exactly once. It does not
  call handler registration again, and no marker from the shutdown period appears later.
- Detailed behavior: verifies [Transport Liveness §6](../spec/29-transport-liveness.en.md#6-connection-loss-and-reconnect).

### Track C — Handle Automatic Discovery And Publisher Lifecycle

#### PS-D1 Discover A Publisher Without An Endpoint

Priority: `P0`

An automatic subscriber does not receive the publisher endpoint through Application configuration or
client input. It discovers and connects to a live descriptor with the same ChannelName.

**Verification question:** Does a subscriber with no endpoint configured bring an automatic publisher
to ready and receive its event?

- Starting condition: The publisher is ready on port 0 and has published its current descriptor to
  the Location Store.
- Procedure: An automatic subscriber with no endpoint configuration is started, and after the public
  status shows the publisher ready, a marker is published.
- Verification: The ready-publisher count is 1, and the subscriber handler receives the marker
  exactly once. The subscriber's application input has no transport endpoint.
- Detailed behavior: verifies automatic discovery in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

#### PS-D2 Select Only The Live Fanout Publisher Of The Same ChannelName

Priority: `P0`

The Store can hold descriptors for other fanout Channels, RouteMesh, and ClientServer together. A
subscriber must use only a publisher whose descriptor kind and ChannelName both match and that can
still take new work.

**Verification question:** Does the `events` subscriber receive only events from the current `events`
publisher?

- Starting condition: A live `events` publisher, an `audit` publisher, and separate RouteMesh and
  ClientServer roles are all registered in the Store. The two fanout publishers are each ready.
- Procedure: Distinct markers are published on `events` and `audit`.
- Verification: The `events` subscriber's status and handler evidence show only the live `events`
  publisher and its marker. The `audit` event and the other topology roles do not appear.
- Detailed behavior: verifies fanout status in [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state).

#### PS-D3 Converges On Publisher Addition And Orderly Removal

Priority: `P1`

When publishers of the same ChannelName increase or decrease, the subscriber must follow the current
set without restarting its process.

**Verification question:** After publisher B is added and A is removed, do the status and the actual
event source match the current set?

- Starting condition: A single `pub-a` is ready, and the subscriber has received A's baseline event.
- Procedure: `pub-b` is started, a ready count of 2 is confirmed, and both publishers send markers.
  `pub-a` shuts down normally; once it is removed from status, B sends a new marker.
- Verification: While both publishers are ready, both markers are received; after A is removed, only
  B remains ready in status, and B's new markers continue to arrive.
- Detailed behavior: verifies [Transport Liveness §5](../spec/29-transport-liveness.en.md#5-ready-and-failure-determination).

#### PS-D4 Replace A Crashed Publisher With A Replacement

Priority: `P0`

When a publisher crashes, its old descriptor must not be used as the current connection after the
owner lease expires. Once the replacement becomes ready, the subscriber receives new events from it.

**Verification question:** After the crash, does the subscriber exclude the old publisher and receive
the replacement's events?

- Starting condition: `pub-a` is ready, and the subscriber has received a baseline event.
- Procedure: The runner force-terminates `pub-a`. Once the public status shows the old publisher
  not-ready or removed, a replacement is started; once it is ready, a new marker is published.
- Verification: Only the replacement appears ready in status, and the subscriber receives the new
  marker exactly once. Events from the crash window are not replayed.
- Detailed behavior: verifies [Transport Liveness §5](../spec/29-transport-liveness.en.md#5-ready-and-failure-determination)
  and [§7](../spec/29-transport-liveness.en.md#7-location-store-and-host-termination).

#### PS-D5 Keeps An Existing Connection And Recovers During A Store Outage

Priority: `P1`

A Location Store polling failure does not stand in for the liveness of a transport connection that is
already ready. As long as the existing publisher keeps sending normal records, the subscriber can
keep receiving events.

**Verification question:** Does the subscriber receive events from the existing publisher during a
Store outage, and converge on the current descriptor set after the Store recovers?

- Starting condition: The publisher and subscriber are ready and have received a baseline event.
- Procedure: The runner blocks the subscriber's Store access, then the existing publisher sends a
  marker. Store access is restored, and once the public fanout status converges to the current state,
  a new marker is sent.
- Verification: Both the marker during the outage and the marker after recovery are each received
  once. A Store outage alone does not immediately turn the existing publisher not-ready.
- Detailed behavior: verifies [Transport Liveness §7](../spec/29-transport-liveness.en.md#7-location-store-and-host-termination).

#### PS-D6 Reconnects Even When A Port-0 Restart Changes The Endpoint

Priority: `P1`

A port-0 publisher's actual port can change on restart. An automatic subscriber must not pin the
previous endpoint — it must follow the current descriptor.

**Verification question:** Does the subscriber receive new events after the publisher's actual port
changes, with no endpoint reconfiguration?

- Starting condition: A port-0 publisher and a subscriber with no endpoint input are ready.
- Procedure: The first actual port is recorded from the publisher's public listener status, then it
  shuts down normally. The same role is restarted on port 0; once a different actual port and
  subscriber readiness are confirmed, a marker is published.
- Verification: Both actual ports are nonzero and different from each other. The subscriber's
  application configuration is unchanged, and the new marker is received exactly once.
- Detailed behavior: verifies [Network Listener Identity §4](../spec/10-network-listener-identity.en.md#4-how-to-confirm-the-port).

#### PS-D7A Isolate A Slow Fanout Status Observer

Priority: `P1`

Even if one status observer processes its callback slowly, publisher connections, event dispatch, and
other observers must keep progressing.

**Verification question:** While a slow observer is waiting, do the normal observer and the fanout
handler still process the current state?

- Starting condition: The subscriber opens a slow observer and a normal observer, and the slow
  observer's first callback is made to wait on an application signal.
- Procedure: Publishers are added and removed, and a business event is sent. The normal observer and
  handler evidence are confirmed, then the slow observer is canceled.
- Verification: The normal observer provides the latest status, and the handler processes the event
  exactly once. If the slow observer's sequence has a gap, `GetStatus` can restore the current state,
  and canceling it does not terminate the other observer.
- Detailed behavior: verifies [Runtime Monitoring §3](../spec/24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

#### PS-D7B Changing A Manual Endpoint Does Not Change Automatic Status

Priority: `P1`

Automatic and manual subscribers are separate connection modes. A manual Channel's endpoint change
must not be reflected in an automatic Channel's publisher set.

**Verification question:** Does an automatic Channel's status and delivery hold steady even as a
manual endpoint is added and removed?

- Starting condition: An automatic `events` subscriber and a separately named manual subscriber are
  each ready.
- Procedure: An endpoint is added and removed through the manual subscriber's public connection
  handle. A marker is sent from the automatic publisher both before and after.
- Verification: The automatic subscriber's ready-publisher identity is preserved, and both markers
  are received. The manual change alone is not required to leave a mark on the automatic status
  sequence.
- Detailed behavior: verifies mode separation in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

### Track D — Verify Manual Mode And Startup Validation

#### PS-E1 Use Only A Manual Endpoint Without A Store

Priority: `P0`

A manual subscriber connects only to the endpoint the Application specifies, and works even without a
Location Store.

**Verification question:** Does a manual subscriber, with no Store, receive only the events of the
publisher it was given?

- Starting condition: A separate publisher is started on a fixed endpoint, and the subscriber is
  configured with only that endpoint.
- Procedure: Once the subscriber is ready, normal delivery and late non-replay are run using
  `before-late` and `after-ready` markers.
- Verification: The marker after readiness is received, and the marker before readiness is not
  replayed. No Store process or descriptor is used.
- Detailed behavior: verifies manual mode in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

#### PS-E2A Reject A Missing Store For An Automatic Subscriber At Startup

Priority: `P0`

An automatic subscriber with no endpoint needs a Location Store for publisher discovery.

**Verification question:** Does a host that registers an automatic subscriber with no Store exit with
a configuration error?

- Starting condition: A negative host registers only an endpoint-less subscriber, with no Location
  Store registered.
- Procedure: The runner starts the host and checks the process terminal and health.
- Verification: The host does not expose a listener or ready status, and exits with a public
  configuration error.
- Detailed behavior: verifies the prerequisite in [Framework API §7](../spec/06-framework-api.en.md#7-logical-multicast-completion).

#### PS-E2B Reject Mixing Automatic And Manual Mode In One Registration

Priority: `P0`

If one subscriber registration uses both Store discovery and an explicit endpoint at the same time,
the connection source becomes ambiguous.

**Verification question:** Does a host that configures both modes together exit with a startup
configuration error?

- Starting condition: A negative host's single subscriber registration specifies both automatic mode
  and a manual endpoint together.
- Procedure: The runner starts the host.
- Verification: The host exits with a configuration error before starting any background connection.
- Detailed behavior: verifies mode validation in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

#### PS-E2C Reject A Missing Or Duplicated Automatic Publisher Identity

Priority: `P0`

An automatic publisher must choose exactly one of a fixed RID or automatic allocation.

**Verification question:** Does startup fail when the publisher identity is left unselected, or when
both approaches are chosen together?

- Starting condition: Two negative hosts are created — one that omits both RID approaches, and one
  that sets both.
- Procedure: The runner starts each host in turn.
- Verification: Both hosts exit with a public configuration error matching the cause, before the
  listener binds.
- Detailed behavior: verifies publisher identity in [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

### Track E — Verify Per-Publisher Liveness

#### PS-F1 Automatic And Manual Publishers Converge To Ready

Priority: `P0`

Descriptor discovery or a successful connect return alone cannot determine that events can be
received. The public fanout status's Ready reflects the first normal application record or liveness
beacon as well.

**Verification question:** Do automatic and manual subscribers mark each publisher Ready and then
process the first event normally?

- Starting condition: Both subscribers start before the publisher, and their public status is
  confirmed not ready.
- Procedure: The automatic publisher and the manual publisher are each started. As soon as each
  status becomes ready, a marker is published.
- Verification: Each subscriber receives its own publisher's marker exactly once. A publisher is not
  used as a ready target before connection. The scenario does not require every brief intermediate
  state to have been seen by the observer.
- Detailed behavior: verifies [Transport Liveness §4](../spec/29-transport-liveness.en.md#4-classic-fanout)
  and [§5](../spec/29-transport-liveness.en.md#5-ready-and-failure-determination).

#### PS-F2 Isolate One Publisher's Receive Disconnection From Another

Priority: `P0`

A subscriber tracks a separate connection and liveness deadline per publisher. Even if records stop
arriving from one publisher, another publisher must keep its ready state and delivery.

**Verification question:** When only publisher B's receipt is blocked, does A remain ready and
continue delivering events?

- Starting condition: A and B of the same ChannelName are both ready at one subscriber.
- Procedure: The runner blocks only the network from B to the subscriber. A keeps publishing markers.
  It waits until B's public status becomes not-ready, then releases the block.
- Verification: While B alone is excluded from the ready list, A's markers keep being processed. The
  host as a whole does not become Error. B becomes ready again after reconnect and delivers new
  markers.
- Detailed behavior: verifies per-publisher liveness in [Transport Liveness §4](../spec/29-transport-liveness.en.md#4-classic-fanout).

#### PS-F3 Reject A Reserved Liveness Topic On Application Publish

Priority: `P0`

The framework distinguishes the exact topic it uses for fanout liveness from Application events. The
exact reserved value is rejected, but a longer topic sharing the same prefix must not also be
forbidden.

**Verification question:** Is the exact reserved topic an argument error, while a topic with a longer
prefix is delivered normally?

- Starting condition: The publisher and subscriber are ready, and the subscriber has registered a
  typed event handler.
- Procedure: The public publish API is used once with the exact reserved topic. It is followed by a
  normal event published with a topic that appends a byte to the same prefix.
- Verification: The first call ends with a public argument error before transport admission, and the
  handler does not run. The second event is processed once by the handler. No private beacon frame is
  built directly in the E2E.
- Detailed behavior: verifies the reserved topic in [Transport Liveness §4](../spec/29-transport-liveness.en.md#4-classic-fanout).

#### PS-F4 Reflects Orderly Disconnect Before The Peer Deadline

Priority: `P1`

A subscriber that receives a normal shutdown signal must exclude the publisher from the ready list
without waiting for the fixed 15-second peer deadline.

**Verification question:** Is a publisher's orderly shutdown reflected in public status immediately,
while other publishers remain?

- Starting condition: Publishers A and B are both ready.
- Procedure: The runner shuts down A normally and observes status within the common readiness
  timeout. A marker is published from B.
- Verification: A drops out of the ready list before the fixed 15-second deadline, and B stays ready
  and delivers its marker exactly once.
- Detailed behavior: verifies [Transport Liveness §5](../spec/29-transport-liveness.en.md#5-ready-and-failure-determination).

#### PS-F5 Keeps Liveness During Unsubscribed Traffic

Priority: `P0`

Even if the subscriber does not process a particular topic's Application events, it must still
separately receive the Framework's liveness record. Otherwise, a healthy publisher could be
disconnected after 15 seconds.

**Verification question:** Does a publisher stay Ready past the peer deadline even while only an
unsubscribed topic is being published?

- Starting condition: The subscriber subscribes only to `events.b`, and the publisher is ready.
- Procedure: The publisher periodically sends `events.a` events for a verification window longer than
  the peer deadline. The verification window is computed as the fixed 15-second deadline plus a
  runner tolerance.
- Verification: There is no handler evidence for `events.a`, but the publisher status stays ready.
  A subsequent `events.b` marker is processed exactly once.
- Detailed behavior: verifies [Transport Liveness §2](../spec/29-transport-liveness.en.md#2-fixed-times-and-public-api-boundary)
  and [§4](../spec/29-transport-liveness.en.md#4-classic-fanout).

### Track F — Handle An Event With No Handler

#### PS-C1 Drop An Unregistered Packet Name

Priority: `P0`

If the subscriber has no packet handler, that event cannot be delivered to an Application handler.
Dispatch of other normal packets must continue.

**Verification question:** Does a packet with no handler appear as `no_handler/drop` in the public
observer, while the next normal event is still processed?

- Starting condition: The subscriber registers a normal packet handler and a public message-flow
  observer, and the publisher is ready.
- Procedure: A packet name with no handler is published, followed by a normal packet.
- Verification: The first event has no handler evidence, and the public observer provides
  `no_handler/drop` exactly once. The normal event is processed exactly once by its handler.
- Detailed behavior: verifies [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout)
  and [Message Flow Tracing §2.2](../spec/26-message-flow-tracing.en.md#22-the-public-behavior-recorded).

## 5. Completion Criteria

- Every scenario uses only public fanout publish, status/observer, and application evidence from the
  role servers.
- Raw frames, private descriptors, socket monitors, and protocol-negative publishers are not used in
  E2E assertions.
- Ready and reconnect are confirmed by bounded polling of public status, cross-checked against actual
  event receipt. Not every intermediate state is assumed to be observed.
- Publish terminal is not used as remote-delivery evidence, and replay of events before connection or
  during a disconnection is not expected.
- Time boundaries are used only to verify the fixed 5-second and 15-second liveness values, with the
  runner tolerance stated explicitly. Pass/fail is not judged by an arbitrary settle sleep.
