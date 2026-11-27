# Authentication Admission and the Login Queue

> Scope: the server Framework for STREAM sessions and the Stream Connector
> Issue: #4546
> Shared fixtures: `framework/runtime/conformance/login-admission-v1.json` (stage 1), `login-admission-queue-v1.json` and `admission-queue-messages-v1.json` (stage 2)

When many logins arrive at once, the authentication handler and the DB lookups and Actor creation behind it
spike together. This document defines admission control that protects the authentication handler and the
resources behind it during such a burst. Stage 1 is a per-node limit on concurrent authentications; stage 2 is a
login queue on top of it that shows clients a global position. Both stages are off by default and an application
turns them on.

## 1. Stage 1: Per-Node Concurrent Authentication Limit

### 1.1 Settings

| Setting | Meaning | Default |
|---|---|---|
| `AuthenticationPacketName` | The one packet name handled as the authentication request | None (when unset, this whole section doesn't apply) |
| `MaxConcurrentAuthentications` | Upper bound on authentication requests one node processes at once. A positive integer | The node's effective CPU core count × 8 |
| `AuthenticationRetryDelay` | Time until retry (ms) reported to a rejected request | 500 |

The settings live in the STREAM session registration options. Each language names them following its session
registration option conventions, as fixed by that language's exact interface documents. A limit of zero or less, or
a delay of zero or less, is a configuration error at startup. The limit and the delay are
integers in `1..2147483647` (the delay in ms); a value outside that range is also a configuration error at startup.
The effective CPU core count is the usable processor count the language runtime reports (reflecting container CPU
quota and affinity), and a computed default above `2147483647` uses `2147483647`. A node keeps one in-flight
authentication count and one limit even with several STREAM registrations — registrations that specify different
`AuthenticationPacketName`, `MaxConcurrentAuthentications` or `AuthenticationRetryDelay` values are a configuration
error at startup.

### 1.2 Decision

The decision applies to **Request**-kind packets whose name equals `AuthenticationPacketName`. Send, Response, and
packets with other names are unaffected by this limit.

1. The Framework keeps the existing receive order (ingress permit → packet receive → header parse,
   [Application job queue §3](../01-execution/04-application-job-queue-and-backpressure.en.md)) and compares the node's
   in-flight authentication count with the limit right after the header shows an authentication request, before
   posting it as an application job.
2. If the in-flight count is below the limit, it adds 1 and passes the request on along the existing path. Permits,
   the queue, and handler execution then follow
   [Application job queue and backpressure](../01-execution/04-application-job-queue-and-backpressure.en.md) unchanged.
3. If the in-flight count equals the limit, the request ends immediately with the rejection result in §1.3 without
   running the handler. A rejected request returns the ingress permit it already took exactly once, isn't posted as an
   application job, and causes no Actor creation or lookup and no Location Store read or write.

The in-flight count decreases by 1, exactly once, when an accepted authentication request reaches one of these
terminals.

| Terminal | Meaning |
|---|---|
| `replySuccess` | The handler produced a success reply |
| `replyFailure` | The handler produced a failure reply |
| `handlerException` | The handler ended with an exception |
| `cancellation` | The request's work was cancelled on the server (session end, runtime cancellation) |
| `enqueueRejected` | The application job queue refused to accept it |
| `callbackWithoutReply` | The handler ended without a reply |
| `shutdown` | It ended because the runtime stopped |

The existing request/callback owner that decides the request's terminal performs the release. `replySuccess` and
`replyFailure` are the point at which the reply is submitted to the STREAM send and accepted; callback work after that
point doesn't count. `callbackWithoutReply` is the point at which the callback ends without a reply. A client's request
timeout isn't the end of the server handler, so it doesn't release. When terminal candidates overlap for one request,
only the first one decided releases. No separate timer compensates for a release.

Time the authentication handler spends waiting on Actor creation or DB lookups all counts as in flight. The
in-flight count and the limit are one value in node memory and are never written to the Location Store. No new Spot,
global state, or token is introduced.

### 1.3 Rejection Result

A rejection is sent as an existing Error packet. The service wire doesn't change.

- The Error body is `{"code":"unavailable","message":"Authentication admission is full."}`
  (`Unavailable` in the [Framework error model](../00-foundation/07-framework-error-model.en.md)).
- The Error packet header metadata carries `$zlink.admission.retryAfterMs`. Its value is the relative delay (ms) until
  the retry, written as a decimal string with no sign and no leading zeros, equal to `AuthenticationRetryDelay`. It
  ranges from 0 to 2^31 − 1. An absolute time isn't used, so skew between the server and client clocks doesn't bunch
  resends together.
- Only an `Unavailable` with this metadata is an admission rejection, and the Framework guarantees the handler didn't
  run. Ordinary `Unavailable` errors never carry this metadata. This limited exception is defined by
  [Framework error model §7](../00-foundation/07-framework-error-model.en.md#7-retry-judgment); the ban on retry hints for ordinary
  errors stays.

### 1.4 Observation

The Framework records the in-flight authentication count and the rejection count as
`zlink.host.authentication_admission.in_flight` and `zlink.host.authentication_admission.rejections` in
[runtime metrics §3](../06-observability/02-runtime-metrics.en.md). The in-flight metric observes the admission's own value;
the metric is never a separate authoritative state.

## 2. Connector Admission Wait and Automatic Retry

[Stream Connector §5.8](../../stream-connector/32-stream-connector.en.md#58-admission-wait-and-automatic-retry) defines the
connector's behavior. Applications don't build their own retry loop.

## 3. Stage 2: Login Queue

### 3.1 Purpose and Relationship

Stage 2 is an optional module on top of stage 1. Stage 1 protects authentication and the DB; stage 2 shows clients a
global position and estimated wait. Turning on stage 2 leaves stage 1's per-node limit and decision in place. The same
in-flight count isn't counted in two places: the queue owns only positions and admission tickets, and accepting or
rejecting an authentication request is decided in one place, stage 1.

### 3.2 Behavior

1. One Instance Spot in the mesh owns the queue. Positions live in that Spot's memory; there's no Location Store key
   per waiting client. If the Spot goes away, waiting clients queue again from the start.
2. Before authenticating, a client asks to enter the queue. The queue Spot assigns a position and pushes the position
   and estimated wait to the client until it can be admitted.
3. When it's the client's turn, the Spot issues a signed admission ticket with an expiry. The client sends that ticket
   with its authentication request.
4. Before the authentication handler runs, the Framework checks the ticket's signature and expiry. If either fails, it
   rejects without running the handler. Otherwise the request continues to the stage 1 decision.
5. The connector handles the waiting state, position events, and ticket reuse, so the client application only
   receives callbacks.

The node in-flight counts the queue uses are a non-binding input to the ticket issue rate. Issuing a ticket doesn't
reserve room on a node, and the authority to accept an authentication request stays with stage 1. If a client holding
a ticket is rejected by stage 1, the connector resends it per
[§5.8](../../stream-connector/32-stream-connector.en.md#58-admission-wait-and-automatic-retry), and the ticket stays valid until it expires.

### 3.3 Fixed Values

These values were fixed by the PM decision (11-20). Changing one changes this section and the shared fixture together.

| Item | Default proposal |
|---|---|
| Ticket signature | HMAC-SHA256 with a secret key from server settings. The ticket carries the session RoutingId, the expiry time, and the queue epoch |
| Ticket delivery | Authentication request header metadata `$zlink.admission.ticket` |
| Position push | A Send packet from the queue Spot to the client session (name `$zlink.admission.position`) carrying the position and estimated wait in ms |
| Admission rate | The queue issues tickets for the free room under the stage 1 limits, computed from the in-flight counts each node reports to the queue Spot |
| Connector callbacks | Waiting state changes (`OnAdmissionQueued`) and position changes (`OnAdmissionPosition`) |
| Ticket expiry | 30 seconds from issue, never longer than the queue epoch's lifetime |

Ticket validation and the secret key follow these rules.

- The server validates tickets with the current key and one previous key, so waiting clients holding a ticket
  signed with the previous key aren't dropped during key rotation. The previous key is never used to sign.
- Signatures are compared in constant time. A ticket whose expiry has passed, or whose epoch differs from the current
  queue, is rejected even if its signature verifies.
- The secret key is accepted only as a setting value. The Framework never writes the key to logs, metrics, error
  messages, or traces.

### 3.4 Wire

The queue doesn't change the service wire; it uses only STREAM packets and header metadata. All JSON integers are JSON
numbers, and `ticket` is an RFC 4648 §5 base64url string without padding.

| Direction | Packet | Content |
|---|---|---|
| Server → client, Error for an authentication Request | `unavailable` + header metadata `$zlink.admission.queueRequired` = `"1"` | Stage 2 is on and the authentication request has no valid ticket. The handler doesn't run and the request isn't counted in stage 1's in-flight count |
| Client → server | Request `$zlink.admission.enqueue`, payload `{}` | Enters the queue. The Framework sends it to the queue Instance Spot |
| Server → client, Response to that Request | `{"position": n, "estimatedWaitMs": m}` or `{"ticket": "...", "expiresInMs": t}` | The position and estimated wait, or a ticket when admission is possible right away |
| Server → client | Send `$zlink.admission.position`, `{"position": n, "estimatedWaitMs": m}` | Sent when the position or estimated wait changes, and every 5 seconds (§3.5.4) |
| Server → client | Send `$zlink.admission.ticket`, `{"ticket": "...", "expiresInMs": t}` | Sent once when it's the client's turn |
| Client → server, authentication Request | Header metadata `$zlink.admission.ticket` = the ticket string | Resends the authentication request carrying the ticket |

A missing ticket, a bad signature, an expired ticket, or an epoch that differs from the current queue are all rejected
with the result in the first row; the client isn't told which reason applied. The queue removes a waiting client when
its session ends or when a ticket it was sent expires unused.

The connector handles it as follows.

- On a rejection carrying `$zlink.admission.queueRequired`, it drops any ticket it holds and sends
  `$zlink.admission.enqueue`. It calls `OnAdmissionQueued` on entering the queue and `OnAdmissionPosition` for each
  position it receives.
- On receiving a ticket, it resends the original authentication request with the same packet name, payload, application
  metadata, and correlation, plus `$zlink.admission.ticket`. If stage 1 rejects that request, it waits per
  [Stream Connector §5.8](../../stream-connector/32-stream-connector.en.md#58-admission-wait-and-automatic-retry) and resends with the same ticket.
- The wait ends as in §5.8: at the earlier of the original request deadline and `first rejection time +
  MaxAdmissionWait`. After that it ends with `RequestTimeout` and sends nothing more.
- If the `$zlink.admission.queueRequired` value isn't `"1"`, or a Response or Send payload doesn't match the shapes
  above, only that request ends with `FrameDecodeFailed`; the connection stays open. A malformed Send follows §3.5.5.

### 3.5 Queue Details

These were fixed by the PM decision (11-25, #4546 stage 2). Changing one changes this section,
[Stream Connector §5.8](../../stream-connector/32-stream-connector.en.md#58-admission-wait-and-automatic-retry), and the shared fixture together.

#### 3.5.0 Stage 2 Settings

Stage 2 settings sit in the same STREAM session registration options as stage 1 (§1.1). Each language's exact interface
document sets the language-specific names. If STREAM registrations on one node give different values, that's a
configuration error at startup.

| Setting | Meaning | Default |
|---|---|---|
| `AdmissionQueueEnabled` | Turns stage 2 (the login queue) on. bool | `false` |
| `AdmissionQueueKey` | Current ticket signing key (§3.5.2). Required when stage 2 is on | none |
| `AdmissionQueuePreviousKey` | Previous ticket verification key (§3.5.2). Optional | none |
| `AdmissionQueueMeshName` | RouteMesh that holds the queue Spot (§3.5.1). Omit it when the process has one MeshNode, which is then used. Required when there are several | none |
| `AdmissionGlobalLimit` | Mesh-wide concurrent authentication limit (§3.5.3). Integer in `1..2147483647`, optional | none (per-node budgets only) |

If `AdmissionQueueEnabled` is `true` and `AuthenticationPacketName` or `AdmissionQueueKey` is missing, there's no MeshNode, or
there are several MeshNodes and `AdmissionQueueMeshName` is missing (`Mesh selection is required`, the same check as the
Instance Spot default mesh), or the name of a STREAM registration with stage 2 on isn't 1–255 bytes of UTF-8 (the ticket MAC's
`len(StreamName)` is one byte), that's a configuration error at startup. Every node carries the same value. When it's `false`, the other stage 2 settings aren't read.

#### 3.5.1 Queue Spot and Epoch

- The queue Spot is one Instance Spot in the queue RouteMesh of §3.5.0 (the only MeshNode's mesh, or `AdmissionQueueMeshName`
  when there are several), with Spot ID
  `$zlink.admission-queue/<RouteMeshName>`. Spot IDs are global names in the Location Store, not scoped by mesh, so the
  RouteMesh name keeps RouteMeshes that share a Store from colliding (within the Spot ID length limit). The Framework prepares it by sending the first `$zlink.admission.enqueue` or the first node
  report (§3.5.3) with an `InstanceSpot(...)` intent. Placement follows the existing Instance Spot placement rules; there's
  no separate placement authority.
- The epoch is a nonzero unsigned 64-bit integer that the queue Spot incarnation generates from a CSPRNG when it
  initializes. A new incarnation (re-preparation after the Spot goes away, or re-initialization after relocation)
  generates a new epoch, and all earlier waiting clients are gone.
- Each node receives the current epoch in the Response to its node report. A node rejects tickets whose epoch differs
  from the last epoch it received. A node that hasn't received a report Response yet rejects every ticket (the result
  in the first row of §3.4).

#### 3.5.2 Ticket Format and Signature

- The ticket is 49 bytes: `version (1 byte, 0x01) || expiresAtUnixMs (8 bytes big-endian) || epoch (8 bytes big-endian) || mac (32 bytes)`,
  sent on the wire as base64url without padding (66 characters). A ticket with any other length or version is rejected
  without checking the signature.
- `mac = HMAC-SHA256(key, "zlink-admission-ticket-v1" || 0x00 || len(NodeRid) (1 byte) || NodeRid bytes || nodeLifecycleGeneration (8-byte big-endian) || len(StreamName) (1 byte) || StreamName UTF-8 || len(SessionRid) (1 byte) || SessionRid bytes || expiresAtUnixMs || epoch)`.
  A SessionRid is unique only within one Core STREAM socket (different nodes or listeners can assign the same value), so the
  connection is identified by (node RID, node lifecycle generation, STREAM registration name, SessionRid). None of these are
  in the ticket. The validating node recomputes the MAC with its own values for the connection that received the
  authentication request, so a ticket issued on a different connection doesn't verify.
- The expiry is wall-clock UTC Unix milliseconds. The issuing Spot sets it to `issue time + 30 seconds`. A validating node
  accepts the ticket only when `current wall clock ≤ expiresAtUnixMs + 2000 ms` (2 seconds of allowed clock difference
  between nodes). `expiresInMs` is `expiresAtUnixMs − issue time`.
- Keys come from the settings `AdmissionQueueKey` (the current key, required) and `AdmissionQueuePreviousKey` (optional).
  Each value is a base64url string, and a decoded length outside 32–64 bytes is a startup error. Turning on stage 2
  without a current key is also a startup error. Validation tries the current key, then the previous key, and the
  signature check passes if either matches by constant-time comparison. Keys rotate by changing each node's settings
  and restarting it; the previous key stays configured until every node signs with the new current key.

#### 3.5.3 Node Reports and Ticket Issue

- Every node with stage 2 on sends the queue Spot a `NodeReport` Spot request once per second. It carries the node RID,
  the stage 1 limit, the current in-flight count, the connection identities (STREAM registration name and SessionRid) whose tickets were used since the previous
  report, the connection identities of waiting sessions that ended since the previous report, and, with a global limit, the permit IDs it holds and the highest contiguous finished request sequence (below). The Spot returns the current epoch in
  the Response. These reports are Framework-internal Spot messages, not client wire.
- The Spot treats only reports received within the last 3 seconds as valid. It issues no tickets to sessions on a node
  without a valid report.
- Each waiting client is recorded by its connection identity (node RID, node lifecycle generation, STREAM registration name,
  SessionRid), and the used and ended lists in node reports use the same identity. A ticket can only be used on that session's
  node, so the issue budget is counted per node:
  `budget(N) = max(0, limit − in-flight − tickets issued to N's waiting clients since that report)`. When a report arrives
  and when a client enters the queue, the Spot issues tickets to each node's waiting clients in enqueue order, up to that
  node's budget. Order is therefore FIFO within a node and approximately FIFO across the mesh.
- With a global limit (below), the issue budget includes it too: `budget(N) = min(node budget(N), max(0, AdmissionGlobalLimit − executing permits − unexpired issued tickets not yet used))`.
  Tickets are therefore issued only as permits free up. When `AdmitPermit` answers a connection with `granted=false`, that
  connection's ticket becomes invalid on the spot and the Spot puts that waiting client back at the head of its node's FIFO. If the
  connection enqueues again, it gets its current position, not the same ticket. The Spot considers `granted` only while the
  connection has a currently valid ticket (issued, not expired, not invalidated); otherwise it answers `granted=false`. An
  authentication that comes back with the old ticket after invalidation and before a new ticket is issued is therefore rejected,
  and once a new ticket is issued, the same connection in the same epoch is treated the same regardless of the ticket bytes.
- **Global limit.** When the setting `AdmissionGlobalLimit` (a positive integer, optional) is present, it caps concurrent
  authentication executions across the whole mesh. A ticket is only an admission right; the queue Spot grants the
  execution permit separately.
  - Before a node passes an authentication with a valid ticket to the handler, it sends the queue Spot one `AdmitPermit` Spot
    request carrying the node RID, the node owner lease (`ownerId`, `leaseGeneration`), the SessionRid, the ticket's epoch,
    and `requestSeq`, a request sequence that increases monotonically within the node incarnation; the Spot records it on the permit.
    While `executing permits < AdmissionGlobalLimit` and the epoch is current, the Spot answers with a permit ID (a nonzero
    64-bit value) with `granted`=true; otherwise it answers `granted`=false (§3.5.8). On a rejection the node sends the client
    `unavailable` + queueRequired, as in the first row of §3.4, and that connection waits again per §3.5.5.
  - When the handler ends (success, failure, or cancellation), the node returns the permit with a `ReleasePermit` Spot send. The
    Spot treats repeated returns of the same permit as idempotent.
  - The Spot records the node owner lease of each permit. It checks the leases of the owners holding permits in the Location
    Store, and reclaims every permit of a lease that ended, only in two cases: when an `AdmitPermit` hits the limit, and when,
    while counting the issue budget for a node `report`, a waiter couldn't get a ticket because of the global limit (so permits
    are reclaimed even when the node holding them vanished without releasing them and no `AdmitPermit` arrives). Otherwise it
    doesn't read the Store. A node report (§3.5.3)
    carries the permit IDs the node holds and the highest contiguous `requestSeq` such that every `AdmitPermit` at or below it
    has finished (answered, or abandoned at the default timeout). The Spot reclaims only permits whose `requestSeq` is at or
    below that value and that are missing from the list. A permit whose answer was lost is therefore reclaimed after that
    request's timeout, and reordered answers never reclaim the permit of a request still in flight.
  - `AdmitPermit` waits for the Framework Spot request's default timeout (no new setting or timer). If it fails or no answer
    arrives in that time, the handler doesn't run and the request ends with `unavailable`. With a global limit, nothing is let past the limit when the Spot is absent.
  - Ticket reuse, delayed use reports, report expiry, and epoch changes don't affect the permit count. The cost, with stage 2 and
    the global limit on, is one Spot round trip and one return send per authentication.
  - Every node is set to the same value. The Spot uses the value of the node its incarnation runs on. A new incarnation starts at
    zero permits; nodes holding an earlier incarnation's permits treat them as returned after the report Response with the new
    epoch and don't count them again, so right after an incarnation change the limit can be exceeded briefly, by at most the
    number of permits that were executing or whose answer was in flight at the change (including a grant from the earlier
    incarnation that arrives late). A handler still running on a node whose owner lease ended and whose permits were reclaimed
    counts as the same kind of temporary overshoot.
- The estimated wait `estimatedWaitMs` is `ceil(position × 1000 / r)`, where r is the number of tickets issued in the last
  30 seconds divided by 30 (per second). When r is 0, it's `position × 1000`. The result is clamped to 2^31 − 1. `position` is the number of waiters ahead in the mesh-wide FIFO that **haven't received a
  ticket**, plus 1 (a waiter that has a ticket no longer waits and isn't counted). `estimatedWaitMs` uses this `position`.
- `$zlink.admission.enqueue` is idempotent per SessionRid. If the session is already waiting, the Response carries its
  current position; if it holds an unused, unexpired ticket, the Response carries that same ticket. No new waiting
  entry is created.
- A waiting client is removed in three cases: a report lists its ticket as used, a report lists its session as ended,
  or its ticket expires unused. A ticket can be used several times on the same session until it expires (resends after
  stage 1 rejections); once the authentication handler runs with that ticket, the node reports it as used.

#### 3.5.4 Position Pushes and Re-registration

- The Spot sends `$zlink.admission.position` to a waiting client when its position or estimated wait changes, and every
  5 seconds even when neither changed. The same values can repeat.
- If the connector receives no position or ticket Send for 15 seconds after entering the queue, it sends
  `$zlink.admission.enqueue` again. If the queue Spot is unchanged, the idempotent handling returns the current position;
  if it's a new incarnation, the client re-enters at the back. Re-registration doesn't call `OnAdmissionQueued` again.

#### 3.5.5 Concurrent Authentication Requests on One Connection

- Tickets and the waiting state are per session (connection). When several authentication requests on one connection
  are rejected with queueRequired, the connector sends enqueue once and resends every waiting request with the ticket
  it receives. Position and ticket Sends are therefore handled per connection, with no operation identifier.
- A ticket Send that arrives before the enqueue Response is accepted. Positions that arrive after the ticket (in a
  Response or a Send) are ignored and don't call `OnAdmissionPosition`. Position and ticket Sends that arrive while no
  request is waiting are discarded.
- When a request is waiting and a position or ticket Send arrives whose payload doesn't match the §3.4 shapes, the Send
  carries no operation identifier and can't be attributed to one request. Every authentication request waiting on that
  connection ends with `FrameDecodeFailed`, and the connection closes.

#### 3.5.6 Reserved Names and Metadata Conflicts

- Only the Framework uses `$zlink.` packet names and metadata keys (Stream Connector §4.6). The connector consumes
  `$zlink.admission.position` and `$zlink.admission.ticket` Sends before the application message callbacks and doesn't
  pass them to the application.
- An Error carrying both `$zlink.admission.retryAfterMs` and `$zlink.admission.queueRequired` ends that request with
  `FrameDecodeFailed`. Admission metadata on an Error whose code isn't `unavailable` is ignored, and the request ends
  with that code's usual result.

#### 3.5.7 Connector Callback Order

- A queueRequired rejection doesn't call `OnAdmissionWaiting`. When the connection enters the waiting state (when it
  sends enqueue), `OnAdmissionQueued` is called once, including when a ticket arrives right away.
- `OnAdmissionPosition(position, estimatedWaitMs)` is called for every position received (the position in the Response
  and each position Send).
- If a request resent with the ticket is rejected by stage 1, `OnAdmissionWaiting` is called per Stream Connector §5.8.
  There's no separate callback for leaving the waiting state; each request's completion marks the end.
- On one connection the order is `OnAdmissionQueued` → `OnAdmissionPosition`* → (`OnAdmissionWaiting`*) → request
  completion. Each language's exact interface fixes the exact signatures together with the implementation PR.

#### 3.5.8 Queue Spot Registration and Node Messages

- The queue Spot's Spot type is the Framework Instance Spot type `$zlink.AdmissionQueue`. Among nodes with stage 2 on, only those whose
  Object role in the queue RouteMesh (§3.5.0) is Server register this type (as placement candidates). An Object Client node doesn't
  register it and only sends the messages (the existing Instance Spot intent). If that mesh has no Location Store or its Object role
  is `None`, that's a configuration error at startup. If the mesh has no Server node at runtime, `enqueue` and `report` fail and the
  node rejects every ticket per §3.5.1.
- The messages between the queue Spot and nodes are the four below and follow the existing typed JSON rules
  ([Message model §5](../00-foundation/05-message-model.en.md)). Field names are camelCase, `dec` is an unsigned 64-bit integer
  (0..2^64−1) as a decimal string (no sign, no leading zeros), and `b64` is unpadded base64url. Every field is required except those marked `?`. Unknown fields are ignored;
  a missing required field or a malformed value is a protocol error.
- Positions and tickets for waiting clients have no new delivery path. The Spot carries them as `deliveries` in that node's next
  `report` Response, and the node sends the §3.4 `$zlink.admission.position` and `$zlink.admission.ticket` Sends on its own
  connection. The `delivery` in the `enqueue` Response works the same way.

Common shapes

| Name | Fields |
|---|---|
| `Connection` | `nodeRid` b64 · `nodeLifecycleGeneration` dec · `streamName` string · `sessionRid` b64 |
| `Delivery` | `streamName` string · `sessionRid` b64 · `kind` `"position"`\|`"ticket"` · `position?` int32 (1-based) · `estimatedWaitMs?` int32 · `ticket?` b64 (66 chars) · `expiresInMs?` int32. With `kind` `position`, `position` and `estimatedWaitMs` are required; with `ticket`, `ticket` and `expiresInMs` are |

A response travels in the same application payload envelope as its request, and its packet name is the request name
followed by `.reply` (`$zlink.admission-queue.enqueue.reply`, `.report.reply`, `.admit-permit.reply`). `release-permit` is a
send and has no response.

| Message | Kind | Request | Response |
|---|---|---|---|
| `$zlink.admission-queue.enqueue` | request | `connection` Connection | `epoch` dec · `delivery` Delivery |
| `$zlink.admission-queue.report` | request (every second, §3.5.3) | `nodeRid` b64 · `nodeLifecycleGeneration` dec · `limit` int32 · `inFlight` int32 · `usedTickets` [{`streamName`, `sessionRid`}] · `endedSessions` [{`streamName`, `sessionRid`}] · `heldPermits` [dec] · `resolvedRequestSeq` dec (`"0"` without a global limit) | `epoch` dec · `deliveries` [Delivery] (this node's waiting clients whose position or estimate changed, whose 5 seconds passed, and new tickets) |
| `$zlink.admission-queue.admit-permit` | request (§3.5.3 global limit) | `nodeRid` b64 · `nodeLifecycleGeneration` dec · `ownerId` string · `leaseGeneration` dec · `streamName` string · `sessionRid` b64 · `epoch` dec · `requestSeq` dec | `granted` bool · `permitId?` dec (required and nonzero when `granted`). A rejection is also a Response (`granted`=false); the node then sends the client the first row of §3.4 (`unavailable` + queueRequired) |
| `$zlink.admission-queue.release-permit` | send | `nodeRid` b64 · `nodeLifecycleGeneration` dec · `permitId` dec | — |

## 4. Tests

- All four languages run the shared fixture `login-admission-v1.json`: 12 server scenarios and 11 connector scenarios.
- All four languages run the two stage 2 shared fixtures. `login-admission-queue-v1.json`: 12 ticket vectors
  (§3.5.2), 22 queue Spot scenarios, 10 node scenarios, 7 startup scenarios (§3.5.0), and 11 connector scenarios.
  `admission-queue-messages-v1.json`: 8 valid and 9 invalid node-to-node messages (§3.5.8). Each language test checks
  the exact counts.
- A cross-language contract test of four language servers × four language connectors: no handler run over the limit,
  the rejection code and `$zlink.admission.retryAfterMs`, success after the connector's automatic retry, and no Actor,
  Location Store, or permit use by a rejected request.
- Passing the fixture validator isn't a runtime pass. Each language's runtime tests also check what the fixture doesn't:
  no Actor lookup or Location Store read for a rejected request, exclusion of the Response kind, startup errors for zero
  or negative settings, disconnect and caller cancellation while waiting, and `retryAfterMs` boundaries (0, leading zero,
  sign, 2^31 − 1, and larger).
