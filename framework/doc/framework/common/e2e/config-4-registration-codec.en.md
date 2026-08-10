<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Previous: Pub/Sub](config-3-pubsub.en.md) | [Next: Resilience](config-5-resilience-lifecycle.en.md)
<!-- framework-adapter-nav:end -->

# Config 4 — Handler Registration And Typed Codec

An application handler must be findable via the public registration
method the language offers, and if the same dispatch key is
duplicated, the host must fail before it can receive a message. A
typed payload is handled as JSON unless configured otherwise, and a
codec extension for a different wire format is registered only once
at the root when needed. This config verifies that registration and
codec selection keep this contract even across real processes.

The E2E client calls the server's application endpoint. The server
endpoint starts a request or send via the public Channel API, and the
target handler and codec extension leave application evidence. The
client doesn't directly read the Framework registry, encoded payload,
or private dispatch table.

## 1. Verification Scope

- Handler scan in a language offering runtime reflection, and explicit
  registration in every language
- Per-dispatch handler scope and dependency lifetime
- Filter before/after order and short-circuit result
- Startup validation of a duplicate dispatch key
- Default typed JSON that needs no separate registration
- Protobuf/MessagePack extension registered once at the root, and the
  result of a codec mismatch
- The application value of `framework-json-v1` that must be kept
  consistent across the five languages

C++, which doesn't offer runtime reflection, uses compile-time types
and explicit builder registration. A reflection helper isn't added to
imitate RC-A1's scan in C++. This is a per-language expression
difference the formal spec fixes
([Framework API §8](../spec/06-framework-api.en.md#8-handler-registration-and-dispatch)),
not a missing feature.

## 2. Deployment Configuration

| Role | Count | What it does and why it's separate |
|---|---:|---|
| registration server | 1 | The `echo` Channel Server on `registration-mesh`. Provides a handler registered via scan and explicit registration, filter, and dependency evidence endpoint. |
| codec server | 1 per scenario | Registers JSON or the specified codec extension at the root. Provides per-codec encode/decode counts and the value the handler restored as application evidence. |
| caller server | 1 | Receives the client's HTTP request and starts a public Channel request or send. Uses a manual endpoint to remove discovery conditions beyond registration/codec. |
| E2E client | 1 | Calls the caller's and role server's application endpoint with the per-language public HTTP client. |

Registration handlers use different packet names but produce
equivalent-meaning results. On receiving `EchoReq.Value`, they return
`echo:<value>` in `EchoRes.Value`, and `EchoMsg` records a marker and
value into evidence once. The codec scenario doesn't use packet name
as a codec-selection means — it selects the extension matching the
payload type.

## 3. Common Run And Judgment Method

The runner creates a fresh process, port, evidence marker, and log
directory per scenario. It starts the client after the role server's
health and public RouteMesh status become ready. A startup-failure
scenario confirms both the fact that the listener never became ready
and the process's public configuration error.

Handler and codec call counts are recorded by the application in the
public registration/extension callback and provided via the public
evidence endpoint. Encoded bytes, private registry entries, and
reflection metadata aren't used for judgment. The file log is only
used to diagnose failure.

## 4. Scenarios

### Track A — Handler Registration And Dispatch

#### RC-A1 Register Handlers Through Each Language's Public Scan Surface

Priority: `P0`

A language offering runtime reflection can find a handler in an
assembly, module, or package the application specifies. When an
annotation or attribute is the scan marker in an exact interface, its
metadata must also become the exact dispatch key. A language without
that surface isn't required to use the same syntax.

**Verification question:** In a language supporting scan, does a
request/send handler registered through the scope and metadata defined
by its exact interface process the specified packet without explicit
registration?

- Start condition: Register only the assembly, module, package, or
  provider scan scope exposed by the language's exact interface. A
  language offering annotation/attribute markers also places a marked
  handler variant in that scope. No variant is added to the explicit
  builder.
- Procedure: The client sends one request and one send for the scope
  variant and, when the exact interface provides it, the
  annotation/attribute variant.
- Verification: Each request receives the exact `EchoRes`, and each
  send marker is recorded once in handler evidence. A control handler
  outside the scan scope doesn't run. A language without the metadata
  surface records only that variant as `not-applicable` and adds no
  substitute helper. The C++ runner records the whole scan scenario as
  `not-applicable` and runs RC-A3 as mandatory instead.
- Detailed behavior: verifies the boundary between runtime reflection
  and C++ explicit registration from
  [Framework API §8](../spec/06-framework-api.en.md#8-handler-registration-and-dispatch)
  and the per-language expression from
  [Public Contract Governance](../spec/00-public-contract-governance.en.md).

#### RC-A3 Register A Handler Explicitly

Priority: `P0`

Explicit registration fixes the handler type and dispatch key directly
into startup configuration, regardless of reflection availability. In
C++, this is the default registration path.

**Verification question:** Do a request/send handler specified via the
public builder produce the same result in every language?

- Start condition: The registration server registers only the
  explicit-variant handler via the public builder.
- Procedure: The client sends one request and one send of the explicit
  variant each.
- Verification: The request receives a reply matching the input
  marker, and the send handler runs once. The result comes with no
  scan and no private registry mutation.
- Detailed behavior: verifies explicit registration from
  [Framework API §8](../spec/06-framework-api.en.md#8-handler-registration-and-dispatch).

#### RC-A4 Split The Dependency Scope Per Dispatch

Priority: `P1`

A Channel handler and filter run in a new scope per dispatch. If the
same scoped dependency is shared across different requests, one
request's mutable state could be exposed to another request.

**Verification question:** Do consecutive requests use different
dispatch scopes while sharing the application singleton?

- Start condition: The handler records the scoped-dependency ID and
  application-singleton ID into the reply and evidence.
- Procedure: The client runs 20 requests with different markers in
  sequence.
- Verification: All 20 scoped-dependency IDs differ, and the singleton
  ID is the same for all. If that language's public DI offers scope-
  disposal observation, the scope is cleaned up exactly once for each
  of normal, handler-failure, and cancellation repetitions. A language
  with no public disposal observation only verifies instance
  separation.
- Detailed behavior: verifies Channel dispatch scope from
  [Framework API §8.2](../spec/06-framework-api.en.md#82-handler-execution-object-and-dependency-lifetime).

#### RC-A5 Filter Order And Short-Circuit Result

Priority: `P1`

Several filters must pass in front of the handler in registration
order, and return in the reverse order once the handler finishes. A
request a filter didn't call `next` on must not look like a normal
business reply.

**Verification question:** Do three filters run in the defined order,
and does a short-circuit request end in `Rejected`?

- Start condition: Register filters `F1`, `F2`, `F3` and the handler in
  this order. A separate packet is controlled by marker so `F2`
  doesn't call `next`.
- Procedure: Send one normal request and one short-circuit request
  each.
- Verification: The normal evidence order is `F1-before, F2-before,
  F3-before, handler, F3-after, F2-after, F1-after`, and the handler
  runs once. The short-circuit request ends in `Rejected`, and `F3`
  and the handler don't run.
- Detailed behavior: verifies filter order and the request short-
  circuit contract from
  [Framework API §8.1](../spec/06-framework-api.en.md#81-handler-filter).

#### RC-A6 Reject A Duplicate Dispatch Key At Startup

Priority: `P0`

Registering the same message kind and packet name twice on the same
owner makes it undecidable which handler to run. Framework must reject
the configuration at startup, not defer it until the first message
arrives.

**Verification question:** Does a host with a duplicate handler key
shut down with a configuration error before publishing the listener as
ready?

- Start condition: Add two handlers with the same ChannelName, request
  kind, and packet name to the negative host via the public
  registration method.
- Procedure: The runner starts the negative host and collects the
  process terminal and health-endpoint status. It then starts a normal
  control host that differs only in packet name.
- Verification: The negative host doesn't publish a listener or ready
  status, and exits with a configuration error. The normal control
  host becomes ready and processes both packets once each.
- Detailed behavior: confirms duplicate-registration verification from
  [Framework API §8](../spec/06-framework-api.en.md#8-handler-registration-and-dispatch)
  and
  [§14](../spec/06-framework-api.en.md#14-startup-validation).

### Track B — Typed Payload Codec Selection

#### RC-B1 Use Default JSON Without Separate Registration

Priority: `P0`

JSON is the default codec for a typed message. If the application had
to register a codec per message type, the default serializer
responsibility would leak to the caller.

**Verification question:** Are typed JSON request and send processed
correctly with no codec extension registered at all?

- Start condition: Caller and codec server register no codec extension
  and only register a JSON DTO handler.
- Procedure: Send a request and a send each, containing a nullable
  field, string enum, signed 64-bit value, and bytes.
- Verification: The handler restores every application value, and the
  request reply and send evidence match the input. Per-message-type
  codec registration count is `0`.
- Detailed behavior: verifies the default typed JSON contract from
  [Framework API §9](../spec/06-framework-api.en.md#9-codec) and
  [Message Model §1](../spec/04-message-model.en.md#1-typed-messages).

#### RC-B2 Use Typed Codec Extensions Registered At The Root

Priority: `P0`

An application using Protobuf or MessagePack registers the official
extension once at the root instead of passing an encoder per message.
When a payload type matches an extension, Framework selects that codec.

**Verification question:** Once Protobuf and MessagePack extensions are
each registered once at the root, are the corresponding DTO request and
send processed through the selected extension?

- Start condition: Run the Protobuf and MessagePack variants with fresh
  roots. In each variant, caller and codec server register the relevant
  official extension once at the root.
- Procedure: Send one DTO request and one DTO send for each variant.
- Verification: Each reply and send evidence matches the input. The
  extension's encode/decode callbacks each run once per message. No
  codec option is passed at the handler call site, and neither packet
  name nor Channel configuration is used as the codec selector.
- Detailed behavior: verifies root codec extension and type-based
  extension selection from
  [Framework API §9](../spec/06-framework-api.en.md#9-codec).

#### RC-B4 Use Multiple Codecs Together At One Root

Priority: `P0`

Even if one server provides JSON, Protobuf, and MessagePack business
together, one codec's registration must not change another payload
type's selection.

**Verification question:** Even sending three payload types mixed
together, is each type processed exactly once via default JSON or the
matching extension?

- Start condition: Register Protobuf and MessagePack extensions at the
  caller's and server's root, plus a JSON handler.
- Procedure: Send 20 each of JSON, Protobuf, and MessagePack requests
  in rotation, and one send of the same type each.
- Verification: The application values of all 60 requests and 3 sends
  are preserved. The extension callback count matches that payload
  type's message count, and neither extension runs on a JSON message.
- Detailed behavior: verifies type-based codec selection and JSON
  fallback from
  [Framework API §9](../spec/06-framework-api.en.md#9-codec).

#### RC-B5 Ends In `ProtocolError` When There's No Receive Codec

Priority: `P1`

If the envelope declares a non-JSON content type but the receiver has
no matching extension, it must not fall back to reinterpreting it as
JSON. A wrong fallback processes the payload as a different value or
produces a handler exception.

**Verification question:** Does sending a request with a Protobuf
extension registered only on the sender return `ProtocolError` without
the receiver handler running?

- Start condition: Register a Protobuf extension on the caller and
  only default JSON on the server. A normal JSON handler is also
  registered.
- Procedure: Send one Protobuf request, then send one JSON request.
- Verification: The Protobuf request ends only once, in
  `ProtocolError`, and the Protobuf handler doesn't run. The JSON
  request receives a normal reply. It's a failure if the receiver
  falls back to JSON for a non-JSON payload.
- Detailed behavior: verifies
  [Framework API §9](../spec/06-framework-api.en.md#9-codec) and
  [Error Model §5](../spec/32-framework-error-model.en.md#5-request-completion-and-failure).

#### RC-B6 Five Languages Restore The Same JSON Application Value

Priority: `P0`

Even with different per-language JSON libraries, a public DTO's
application value must be the same. A meaningless byte difference,
like JSON object member order or whitespace, isn't verified.

**Verification question:** Across every directional language
combination, does the `framework-json-v1` fixture round-trip to the
same typed value?

- Start condition: Each language's server and caller register the same
  DTO semantics and packet name with no separate codec extension.
- Procedure: Send a golden request containing property-name
  casing, a string enum, a signed 64-bit decimal string, padded
  Base64 bytes, a 32-bit JSON number, a finite floating-point value,
  and a nullable field, in each language's direction. Also send
  unknown-field, duplicate-field, and missing-required-field fixtures
  each.
- Verification: The golden request restores to the same typed
  application value, and the reply also has the same meaning. Unknown
  fields are ignored. A duplicate field and a missing required field
  each end in `ProtocolError` before the handler. Matching re-encoded
  JSON bytes, whitespace, or member order isn't required.
- Detailed behavior: verifies `framework-json-v1`'s cross-language
  semantics from
  [Message Model §2](../spec/04-message-model.en.md).

## 5. Completion Conditions

- `P0` items RC-A1, RC-A3, RC-A6, RC-B1, RC-B2, RC-B4, and RC-B6 pass
  in every supporting language. C++, which doesn't support RC-A1,
  records `not-applicable` and the formal spec basis in the feature
  map.
- The client calls only the role server's public business/evidence
  endpoint. It doesn't directly read Framework registration, the codec
  registry, or encoded payload.
- Handler and extension callback count only uses evidence the
  application recorded in a public callback.
- Readiness and handler completion are confirmed via public status or
  a bounded application-evidence wait.
- On failure, the client result, handler/codec application evidence,
  and role server log are preserved. The log is investigation
  material, not a pass condition.
