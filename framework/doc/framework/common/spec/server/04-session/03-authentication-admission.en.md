# Authentication Admission and the Login Queue

> Scope: the server Framework for STREAM sessions and the Stream Connector
> Issue: #4546
> Shared fixture: `framework/runtime/conformance/login-admission-v1.json`

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
a delay of zero or less, is a configuration error at startup.

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
| Server → client | Send `$zlink.admission.position`, `{"position": n, "estimatedWaitMs": m}` | Sent when the position changes |
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
  above, only that request ends with `FrameDecodeFailed`; the connection stays open.

## 4. Tests

- All four languages run the shared fixture `login-admission-v1.json`: 12 server scenarios and 7 connector scenarios.
- A cross-language contract test of four language servers × four language connectors: no handler run over the limit,
  the rejection code and `$zlink.admission.retryAfterMs`, success after the connector's automatic retry, and no Actor,
  Location Store, or permit use by a rejected request.
- Passing the fixture validator isn't a runtime pass. Each language's runtime tests also check what the fixture doesn't:
  no Actor lookup or Location Store read for a rejected request, exclusion of the Response kind, startup errors for zero
  or negative settings, disconnect and caller cancellation while waiting, and `retryAfterMs` boundaries (0, leading zero,
  sign, 2^31 − 1, and larger).
