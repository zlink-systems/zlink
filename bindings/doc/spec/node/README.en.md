---
title: "Node / TypeScript Binding Implementation Blueprint"
---

<!-- bindings-nav:start -->
[Spec index](../README.md) | [Previous: Java](../java/README.md) | [Next: Python](../python/README.md)
<!-- bindings-nav:end -->

# Node / TypeScript Binding Implementation Blueprint

> **What this chapter defines** — the `contracts`/`runtime` shape the
> Node/TypeScript library must have, and the package export boundary.

This document defines the shape the Node/TypeScript library must have. It is
not an exhaustive list of every class or type member. The concrete public
contract is the package-root export declared by `bindings/node/src/index.ts`,
the `package.json` exports, and the generated `.d.ts` surface.

A Node/TypeScript implementation is considered aligned when the source
package tree, package exports, `.d.ts` types, tests, samples, perf runners,
and runtime behavior follow this blueprint and map stable
`core/include/zlink.h` capabilities into TypeScript-idiomatic APIs.

This README describes the Node/TypeScript binding shape after it is aligned
to the shared policy in `../README.md`, and it also serves as the guide for
the Node refactoring work. During the refactor, use this document to decide
where each public contract, runtime implementation, native bridge helper,
test, sample, and perf import belongs. Once the Node binding is declared
aligned, generated declarations, package exports, tests, samples, perf, and
runtime behavior all match this document.

The Node refactor is a breaking cleanup. It does not keep compatibility
shims, deprecated wrappers, duplicate construction paths, or runtime
re-export aliases only to preserve the pre-refactor public surface.

This binding follows the shared bindings architecture map with TypeScript
naming conventions. It uses lower-case `contracts` and `runtime` source
folders, and package exports decide what is public. It does not copy
capitalized .NET or C++ folder names into the Node package as-is.

Node follows the [.NET design shape](../dotnet/README.md) after alignment.
Native-backed resource behavior is described by public contract interfaces
and types under `src/zlink/contracts`; runtime implementations live under
`src/zlink/runtime` and are obtained through package-root factory functions.
Concrete value classes, DTO-shaped objects, enums, literal unions, results,
and errors stay in the contract source.

The first code a reviewer should read is the public contract under
`src/zlink/contracts`, the same way the [.NET binding blueprint](../dotnet/README.md)
starts from `Contracts/`. Runtime files must implement that contract; they
must not be the place where callers discover new behavior.

The binding uses the same architecture map while keeping TypeScript and Node
conventions: lower-case folders, camelCase methods, PascalCase public types,
structural interfaces where TypeScript makes that clearer, small DTO-shaped
results as plain objects where that is clearer, and package-root exports as
the consumer surface. It does not copy C# interface prefixes, namespace
casing, or file names when a TypeScript idiom is clearer.

| Section | Covers |
|---|---|
| [Public Contract Source](#public-contract-source) | Export projection, contract source location, package boundary |
| [Repository Layout](#repository-layout) | The aligned directory tree and lower-case folder rules |
| [API Change Workflow](#api-change-workflow) | New-mapping/refactor procedure, shortcuts that must be removed |
| [Library Shape](#library-shape) | Resources/roles that need interface-first definitions |
| [Contract / Runtime Placement Rules](#contract--runtime-placement-rules) | The boundary between public declarations and runtime implementation |
| [Contract Category Map](#contract-category-map) | Category-to-folder mapping |
| [Contract File Layout](#contract-file-layout) | Files by category under `contracts/` |
| [Runtime File Layout](#runtime-file-layout) | Files by category under `runtime/`, and alignment-failure examples |
| [Construction Entry Points](#construction-entry-points) | The list of package-root factory functions |
| [Function Naming Rules](#function-naming-rules) | camelCase, canonical action names, handler-registration rules |
| [Canonical Interface Rules](#canonical-interface-rules) | `recv` signatures, builders, exceptions such as `publishAsync` |
| [Public Entry Shape](#public-entry-shape) | Domain grouping of the package entrypoint |
| [64-bit Byte HWM and Monitoring Contract](#64-bit-byte-hwm-and-monitoring-contract) | `bigint` HWM representation and monitor snapshot fields |
| [Required Capability Coverage](#required-capability-coverage) | User-facing capabilities that alignment must guarantee |
| [Spot Get-Or-Create](#spot-get-or-create) | The `getOrCreateSpot` contract |
| [Receive and Subscribe Shape](#receive-and-subscribe-shape) | Caller-provided storage and no-data distinction |
| [Error and Validation Policy](#error-and-validation-policy) | Validation timing and error structuring |
| [Performance Policy](#performance-policy) | Hot-path constraints |
| [Implementation Checklist](#implementation-checklist) | Pre-alignment checks and required verification commands |
| [Actor and Spot Route Results](#actor-and-spot-route-results) | Route result types, and Actor-targeted send/request |

## Public Contract Source

- Public contract projection: `bindings/node/src/index.ts`, the generated
  `.d.ts`, and `package.json` exports.
- Contract source: `bindings/node/src/zlink/contracts/`.
- Package projection: symbols exported from the package entrypoint and
  declared in the published TypeScript definitions.
- Internal implementation: native addon modules, private source modules,
  N-API handles, callback trampolines, request progress helpers, converters,
  and raw part-loop helpers.
- Package boundary: `package.json` exports expose only documented public
  entrypoints.
- Documentation role: this README defines shape and semantic coverage. The
  package entrypoint and declarations own the exact public member list.

Deep imports into source files or native bridge modules are not public API.

## Repository Layout

Use these paths consistently when changing the Node/TypeScript binding.

- Public entrypoint: `bindings/node/src/index.ts`.
- Contract source: `bindings/node/src/zlink/contracts/`.
- Runtime implementation: `bindings/node/src/zlink/runtime/`.
- Native bridge/artifacts: `bindings/node/src/zlink/runtime/native/`,
  `bindings/node/native/`, `bindings/node/prebuilds/`, and generated runtime
  loading code.
- Generated output: `bindings/node/dist/`. Not contract source.
- Codec package: not provided. The Node binding keeps only the raw `Message`
  and byte payload APIs.
- Tests: `bindings/node/tests/`.
- Samples: `bindings/node/samples/`.
- Perf: `bindings/node/perf/`.

- `package.json` exports and the generated `.d.ts` file must agree with the public entrypoint. Do not document or test deep source imports as public API.
- `index.ts`, the published `.d.ts` file, and `package.json` exports are the contract's TypeScript package projection.
- Do not expose deep source paths as public API unless they are deliberately listed in `package.json` exports.
- Use lower-case source directory names. Do not create `src/zlink/Contracts` or `src/zlink/Runtime`; those names could be mistaken for a public deep-import surface.
- `src/zlink/contracts` owns public TypeScript types, classes, builders, enums, errors, and factory return contracts. The package entrypoint or a runtime factory module owns the factory implementation.
- `src/zlink/runtime` owns native-backed runtime implementations, native addon calls, handle owners, callback trampolines, request progress helpers, marshalling, and platform loading.

The following tree is the aligned implementation structure.

File granularity follows the common policy in `../README.md`. Keep one file
per independent public concept or tight operation/model group. Very small
type aliases, callback types, enum-only files, and pass-through helper
modules are merged into the nearby contract file to keep the public shape
readable.

```text
bindings/node/
+-- src/
|   +-- index.ts
|   +-- zlink/
|   |   +-- contracts/
|   |   |   +-- core/
|   |   |   |   +-- context.ts
|   |   |   |   +-- zlink.ts
|   |   |   |   +-- routing_id.ts
|   |   |   +-- messaging/
|   |   |   |   +-- message.ts
|   |   |   |   +-- received.ts
|   |   |   |   +-- topic_message.ts
|   |   |   |   +-- subscription_event.ts
|   |   |   +-- sockets/
|   |   |   |   +-- socket.ts
|   |   |   |   +-- pair_socket.ts
|   |   |   |   +-- dealer_socket.ts
|   |   |   |   +-- router_socket.ts
|   |   |   |   +-- pubsub_sockets.ts
|   |   |   |   +-- stream_socket.ts
|   |   |   |   +-- socket_options.ts
|   |   |   |   +-- socket_operations.ts
|   |   |   +-- eventing/
|   |   |   |   +-- monitor.ts
|   |   |   |   +-- poller.ts
|   |   |   |   +-- timer.ts
|   |   |   +-- service/
|   |   |   |   +-- spot/
|   |   |   |   |   +-- spot_node.ts
|   |   |   |   |   +-- spot.ts
|   |   |   |   |   +-- actor.ts
|   |   |   |   |   +-- spot_operations.ts
|   |   |   |   |   +-- spot_models.ts
|   |   |   +-- errors/
|   |   |   |   +-- errors.ts
|   |   |   |   +-- results.ts
|   |   +-- runtime/
|   |   |   +-- core/
|   |   |   |   +-- context.ts
|   |   |   |   +-- context_options.ts
|   |   |   |   +-- runtime_info.ts
|   |   |   +-- handles/
|   |   |   |   +-- native_handle.ts
|   |   |   |   +-- lifetime.ts
|   |   |   +-- messaging/
|   |   |   |   +-- message_materializer.ts
|   |   |   |   +-- request_progress.ts
|   |   |   +-- buffers/
|   |   |   |   +-- message_conversion.ts
|   |   |   |   +-- buffer_policy.ts
|   |   |   +-- sockets/
|   |   |   |   +-- socket_base.ts
|   |   |   |   +-- socket_options.ts
|   |   |   |   +-- socket_operations.ts
|   |   |   |   +-- pair_socket.ts
|   |   |   |   +-- dealer_socket.ts
|   |   |   |   +-- router_socket.ts
|   |   |   |   +-- pub_socket.ts
|   |   |   |   +-- sub_socket.ts
|   |   |   |   +-- xpub_socket.ts
|   |   |   |   +-- xsub_socket.ts
|   |   |   |   +-- stream_socket.ts
|   |   |   +-- eventing/
|   |   |   |   +-- monitor_socket.ts
|   |   |   |   +-- poller.ts
|   |   |   |   +-- poll_events.ts
|   |   |   |   +-- timer.ts
|   |   |   +-- options/
|   |   |   |   +-- option_mapping.ts
|   |   |   |   +-- validation.ts
|   |   |   +-- service/
|   |   |   |   +-- spot/
|   |   |   |   |   +-- spot_node.ts
|   |   |   |   |   +-- spot.ts
|   |   |   |   |   +-- actor.ts
|   |   |   |   |   +-- spot_operations.ts
|   |   |   +-- errors/
|   |   |   |   +-- native_errors.ts
|   |   |   +-- native/
|   |   |   |   +-- native.ts
|   |   |   +-- internal/
|   |   |   |   +-- request_pump.ts
|   |   |   |   +-- service_mapping.ts
+-- native/
+-- tests/
+-- samples/
+-- perf/
+-- prebuilds/
+-- dist/
```

The package-root export is the consumer entrypoint. Tests, samples, and perf
import from that entrypoint or another documented package export, not from
`src/zlink/runtime`, native addon modules, or generated private files. If a
symbol appears in the package root or the generated `.d.ts`, a reviewer must
be able to point to its owner under `src/zlink/contracts` or the package-root
entrypoint.

## API Change Workflow

When mapping a new core capability:

1. Add the public symbol to the correct contract source category.
2. Update the package entrypoint, declaration surface, and `package.json`
   projection.
3. Keep native addon calls, N-API handles, and request progress helpers
   behind private modules.
4. Choose a class, interface, type alias, literal union, or plain-object
   shape according to normal TypeScript usage.
5. Add runtime tests and type-surface tests against the package entrypoint.
6. Update samples and perf only through public imports.
7. Confirm the generated `dist` and `.d.ts` output do not expose private
   bridge modules.

When refactoring existing code to this shape:

1. Move public behavior declarations to `src/zlink/contracts/<category>/`.
2. Move native-backed runtime implementations to
   `src/zlink/runtime/<category>/`.
3. Keep native addon loading and N-API calls under
   `src/zlink/runtime/native/`.
4. Replace direct runtime construction in public code with package-root
   factories or contract methods.
5. Remove compatibility exports that expose runtime modules as public API.
6. Remove deprecated wrappers, duplicate overload families, and old naming
   aliases rather than keeping them as shims.
7. Update tests, samples, and perf to import from the package root only.
8. Regenerate declarations and confirm `dist/index.d.ts` carries the
   contract surface, not runtime implementation modules.

The refactor is complete only once the following Node-specific shortcuts are
removed. These are not optional compatibility layers.

- `src/zlink/contracts` does not re-export runtime handle modules.
- Contract files do not import runtime resource classes to describe public
  service models.
- A public runtime aggregate such as `runtime/handles/canonical.ts` does not
  remain the source of public resource behavior. Split those declarations
  into named contract files and resource-named runtime implementation files.
- `src/index.ts` exports package contract names and factories, not runtime
  implementation modules.
- `package.json` does not expose runtime, native, generated, or private
  source subpaths.
- Generated declarations do not mention runtime implementation module paths
  as public types.

For a handoff, a short task statement is enough: refactor the Node binding
according to this README and `../README.md`, use the .NET design shape,
preserve TypeScript naming style, remove compatibility shims, and pass this
document's verification gates.

## Library Shape

This binding feels like a TypeScript package with a native backend.

- Native-backed resource behavior contracts are public TypeScript interfaces
  under `src/zlink/contracts`.
- Native-backed runtime implementations live under `src/zlink/runtime`. They
  are not package exports and not construction entrypoints.
- Public contract files must be readable without opening runtime files. A
  reviewer can understand callable methods, return values, lifecycle, error
  behavior, and builder shape from `contracts/` alone.
- Resource contracts expose `close()` or an equivalent lifecycle method.
- Values such as message, routing id, received metadata, topic message,
  snapshots, options, enums, literal unions, and errors stay concrete or
  structural following normal TypeScript convention.
- Operation builders use public contract interfaces because they hide staged
  native request state and multipart accumulation.
- Native addon handles, raw pointers, callback userdata, request pumps, and
  part-loop sequencing are never exposed.

Do not introduce an interface for a pure DTO/value object only for symmetry.
`Message`, `RoutingId`, `Received`, `TopicMessage`, route results, snapshots,
option objects, enums, literal unions, and errors remain concrete or
structural public values.

Define a public TypeScript interface for each of the following native-backed
resources and roles before writing or exposing a runtime class.

- `Context`.
- Socket roles: common socket behavior, `PairSocket`, `DealerSocket`,
  `RouterSocket`, `PubSocket`, `SubSocket`, `XPubSocket`, `XSubSocket`, and
  `StreamSocket`.
- Eventing roles: `MonitorSocket`, `Poller`, poll event source, `Timer`,
  `Stopwatch`, and `AtomicCounter`.
  `Spot`, `Actor`.
- Operation builders: send, routed send, request, reply, publish, channel
  send/request, SPOT send/request/reply, actor create, actor join, and actor
  join reply builders.
- Callback roles: stream packet handler, monitor handler, poll handler, SPOT
  dispatch handler, route handler, and admission handler.

The runtime class that implements a role may have a private or unexported
name, but the package-root factory and the generated declaration must use
the public contract interface name.

Perf and samples do not rely on undocumented deep import paths to reach
native objects faster.

## Contract / Runtime Placement Rules

- Exported TypeScript classes, interfaces, type aliases, error types, and
  builder contracts belong in `src/zlink/contracts` or the package
  entrypoint.
- Exported package functions, static helper types, convenience method
  contracts, and builder helper contracts belong in contract source when a
  caller can use them directly.
- Factory return types and callable factory signatures belong to the public
  contract. Factory implementation goes in the package entrypoint or a
  runtime factory module so contract files do not import runtime
  implementations.
- JavaScript runtime implementations, native handle owners, request pumps,
  callback adapters, and part-loop helpers belong in `src/zlink/runtime`.
- N-API bindings, native addon handles, marshalling helpers, and platform
  loading code belong in `src/zlink/runtime/native`.
- Package exports and the published `.d.ts` file must project contract
  source; they do not expose runtime modules.
- Runtime concrete classes are construction targets behind package-root
  factories. Callers do not import runtime modules directly.
- Do not export `src/zlink/runtime/*` from `src/zlink/contracts` or
  `src/index.ts`. `src/index.ts` may import runtime modules only to wire
  package-root factories. A runtime implementation type may satisfy a public
  contract, but the exported type name comes from contract source.
- Package-root factories declare contract return types explicitly. For
  example, `createContext(): Context` returns the public contract type even
  though it instantiates a runtime implementation.

## Contract Category Map

`src/zlink/contracts` is the source-ownership map for the package entrypoint
and the published TypeScript declarations.

- `core/`: context, context options, routing id, version/capability lookup
  helpers, and utility contracts.
- `messaging/`: `Message`, received metadata, topic messages, subscription
  events, stream packet data, and builder payload helpers.
- `sockets/`: socket behavior, socket families, typed options, and
  request/reply and publish/subscribe surfaces.
- `eventing/`: monitor, monitor snapshot/event, poller, poll events, timer,
  and public poll helpers.
- `service/`: SPOT node, SPOT handle, topology model, Actor reference, Actor
  lifecycle, and operation builders.
- `errors/`: typed error classes or tagged error domains.
- Enum, flag, result, and literal-union types live in the category that
  defines their meaning. Do not create an `enums` folder just to group them
  syntactically.

## Contract File Layout

Contract source keeps the same classification as the
[.NET binding blueprint](../dotnet/README.md), with TypeScript naming. It
keeps the same conceptual file grouping so a developer who knows the .NET
binding can find the same public concept in Node quickly. The folder map is
shared with .NET, but the names inside it stay idiomatic TypeScript.

- `core/`: `context.ts`, `zlink.ts`, `routing_id.ts`, and core option/value
  files.
- `messaging/`: `message.ts`, `received.ts`, `topic_message.ts`,
  `subscription_event.ts`, and common operation payload types.
- `sockets/`: socket interfaces, socket option types, send/request/reply
  builder contracts, stream packet handler contracts, and socket flags.
- `eventing/`: monitor, monitor event/status, poller, poll events, timer, and
  event handler contracts.
- `service/`: a `spot/` subfolder holding SPOT node, Spot, Actor, topology
  model, and service operation builders. Use named files such as
  `spot_node.ts`, `spot.ts`, `actor.ts`, and `spot_operations.ts`, and group
  model files with their service domain.
- `errors/`: public error classes, result domains, and error-code mapping.

Do not collect public resource behavior into one aggregate `models.ts` or a
runtime-export barrel. Small DTO-shaped objects and literal unions can be
grouped with the contract that gives them meaning, but native-backed
resources and operation builders need named contract files.

## Runtime File Layout

Runtime source follows the runtime classification in the
[.NET binding blueprint](../dotnet/README.md), but holds only
implementation. Node runtime file names use the same lower-case TypeScript
concept names as the contract tree. Do not use a `default_` filename prefix
such as `default_context.ts` or `default_pair_socket.ts`. In this package,
every file under `src/zlink/runtime` is already the native-backed
implementation side of the contract/runtime split, so a file name should
describe the resource or operation it implements, not the fact that it is an
implementation.

- `core/`: `context.ts`, `context_options.ts`, and runtime helper functions
  such as version/capability lookup wrappers.
- `handles/`: native handle owners, lifetime checks, close/dispose state, and
  reference tracking.
- `messaging/`: message materialization, request progress, request
  execution, and multipart progress helpers.
- `buffers/`: message conversion, buffer ownership, copy/borrow policy, and
  pooled/pinned storage helpers.
- `sockets/`: `socket_base.ts`, `socket_options.ts`, `socket_operations.ts`,
  and one implementation file per socket family — `pair_socket.ts`,
  `dealer_socket.ts`, `router_socket.ts`, `pub_socket.ts`, `sub_socket.ts`,
  `xpub_socket.ts`, `xsub_socket.ts`, and `stream_socket.ts`.
- `eventing/`: `monitor_socket.ts`, `poller.ts`, `poll_events.ts`,
  `timer.ts`, and related event materialization helpers.
- `options/`: option validation and native option id/value mapping shared by
  context, sockets, and services.
- `service/`: SPOT node, Spot, Actor, topology, and service operation
  implementations. Use a `spot/` subfolder once the implementation grows
  large enough.
- `errors/`: native error translation and validation helpers.
- `native/`: native addon loading, platform lookup, and N-API binding
  surface.
- `internal/`: only small private glue that does not fit a standard .NET
  runtime classification. Do not put handle ownership, buffer policy, option
  mapping, native declarations, or public resource behavior here when a
  standard classification already exists.

Runtime files may import contract types, but contract files do not import
runtime files. The package root may instantiate native-backed runtime
implementations in factories, but it exports contract names, not runtime
implementation modules.

Category files such as `runtime/sockets/sockets.ts`,
`runtime/service/service.ts`, `runtime/eventing/eventing.ts`, and
`runtime/core/index.ts` are allowed only as small barrels. They may
re-export nearby implementation files or define factory wiring that stays
internal to runtime, but they do not hold native-backed resource class
bodies, operation builders, or marshalling logic. If a reviewer must read a
category aggregate to understand how `RouterSocket`, `SpotNode`, or `Poller`
behaves, the file split is not aligned.

Runtime implementation file names describe the resource or operation they
implement, not the fact that they are native-backed. Use `router_socket.ts`,
`spot_node.ts`, `poller.ts`, and `timer.ts`, not `default_router_socket.ts`,
`default_spot_node.ts`, or `default_poller.ts`.

A shared helper must not become a second public implementation aggregate.
`runtime/internal/*` may own narrow private glue that crosses several
runtime categories, but it does not own public resource behavior and does
not hide a standard .NET runtime category. Native handle ownership belongs
in `runtime/handles`, buffer conversion in `runtime/buffers`, option mapping
in `runtime/options`, native addon declarations in `runtime/native`, and
public resource behavior in a resource runtime file such as
`sockets/router_socket.ts` or `service/spot/spot_node.ts`.

Shared helper files under a category follow the same rule. A file such as
`runtime/sockets/socket_common.ts` may hold narrow socket helper types or a
private base utility, but it does not hold several unrelated concerns at
once. If operation builders, monitor socket behavior, routing helpers,
marshalling helpers, and concrete resource behavior all sit in one file,
that file has become a hidden aggregate and must be split into
`socket_base.ts`, `socket_options.ts`, `socket_operations.ts`, and smaller
internal helpers.

The following shapes are explicit alignment failures.

- `runtime/service/service.ts` holds the `SpotNode`, `Spot`, and `Actor`
  implementations in one file.
- `runtime/eventing/eventing.ts` holds the monitor socket, poll events,
  poller, timer, stopwatch, and counter implementations in one file.
- `runtime/core/context.ts` holds context, context options, and unrelated
  runtime helper implementation in one file.
- `runtime/core/runtime_info.ts` holds a copied implementation prelude, or
  socket/service behavior, just to reach its helper functions.
- `runtime/sockets/socket_common.ts` holds operation builders, monitor
  socket behavior, route helpers, message conversion, and base socket
  behavior all in one large file.
- `runtime/internal/*` owns public resource behavior instead of a private
  helper mechanism.
- `runtime/internal/*` owns handle lifetime, buffer conversion, option
  mapping, native addon declarations, or error mapping that belongs to a
  standard .NET runtime classification.

## Construction Entry Points

Interfaces define behavior; construction is provided by package-root
factories and public contract methods.

- `createContext()` creates a runtime context implementation.
- `Context.createPairSocket()`, `createDealerSocket()`,
  `createRouterSocket()`, `createPubSocket()`, `createSubSocket()`,
  `createXPubSocket()`, `createXSubSocket()`, and `createStreamSocket()`
  create runtime socket implementations.
  Service-layer implementations are created accordingly.
- A `Spot` handle is obtained through `SpotNode.createSpot()`,
  `entrySpot()`, `getOrCreateSpot(...)`, or `spotLookup(...)`. Direct `Spot`
  construction is not public.
- An Actor handle is created through `SpotNode.createActor(...)`. Direct
  Actor construction is not public.
- `createPoller()`, `createTimer()`, and `createTimer(spot)` create eventing
  resources.
- Package-root factory/helper functions such as version, capability lookup,
  strerror, proxy, sleep, and multipart-cleanup helpers are public contract
  functions. The native calls behind them stay in runtime modules.

Direct construction of a native-backed runtime class is not part of the
aligned contract. Factories are the stable construction surface.

## Function Naming Rules

Function names follow the shared binding meaning rules in `../README.md`,
using TypeScript spelling.

- Use `camelCase` for methods and functions.
- Use the same canonical action names as the other bindings, differing only
  in case: `send`, `request`, `reply`, `publish`, `subscribe`,
  `unsubscribe`, `recv`, `recvRouted`, `receiveSubscriptionEvent`,
  `setSendReadyHandler`, `setPacketHandler`, `setDispatchHandler`,
  `getOrCreateSpot`, `sendToChannel`, `requestToChannel`, `sendToSpot`, and
  `requestToSpot`.
- Do not keep an old alias only for compatibility. When a pre-refactor name
  conflicts with the canonical meaning, remove it and expose the canonical
  TypeScript name.
- Do not use `on...` names for handler registration. Use `set...Handler`
  when the API stores or replaces the current handler.
- Do not create operation-start variants such as `sendNoWait`,
  `publishWithFlags`, or `requestAsync`. Keep a single operation name and
  put flag, timeout, callback, and async-submit choices on the builder.

## Canonical Interface Rules

- Data-plane `recv`, routed recv, `subscribe`, and subscription-event
  receive fill a caller-provided `Received`, `TopicMessage`, or
  `SubscriptionEvent` object and return `boolean`.
- Send, routed send, publish, request, reply, SPOT operations, and Actor
  location/session operations return a fluent builder.
- A builder start method takes only the target identity, topic, channel,
  routing id, or request sequence. Payload, flags, timeout, callback, and
  async-submit choices are builder steps.
- SPOT channel-targeted operations use `sendToChannel(...)` and
  `requestToChannel(...)`. SPOT topic publish stays `publish(topic)`.
- Do not add a single-payload shortcut overload sharing a name with an
  operation start method. `send(message)`, `send(routingId, message)`,
  `publish(topic, message)`, `sendToChannel(channel, message)`, and
  `sendToSpot(..., message)` are not public contract members. Callers use
  `send(...).message(message).submit()`.
- Multipart payload is accumulated by repeated `message(...)` calls. A
  `messages(...)` convenience is allowed when it delegates to the same
  builder contract and is declared in contract source.
- A Dealer socket does not expose protocol envelope helpers such as
  `requestFrame(...)` or `reply(requestToken, parts)`. A dealer can start a
  request through `request()`, but it has no API-level peer routing id, so
  it cannot reply to an arbitrary token.
- Node `Buffer` / `Uint8Array` payload input is copied into message-owned
  native storage before the native queue could outlive the call. Do not
  expose or use a borrowed-Buffer send helper such as
  `socketSendBorrowedNoWaitResult`.
- The message payload factory is `Message.from(...)`. The public TypeScript
  contract does not require the caller to use `new Message(...)` to create a
  payload.
- Operation-start naming follows the Function Naming Rules above. A
  builder's terminal method keeps using `submit(...)` even on a
  Promise-returning surface. Do not add a separate `submitAsync` terminal
  name.
- The MeshNode Logical Multicast publisher also provides `publishAsync(...)`
  because Core's one blocking publish call must run outside the Node.js
  event loop. This name applies only to this publisher and does not change
  the async-suffix rule for other binding operations. Payload and metadata
  are copied into binding-owned storage before the worker is queued. An
  `AbortSignal` can cancel the operation only before the Core call starts.
  An abort after the Core call has started does not change the submit
  result and detail of the publish that has already started. A programming
  or system failure remains an exception. No separate timeout option is
  added; Core's MeshNode send timeout applies.
- `publishAsync(...)` returns `Promise<MeshPublishResult>`. `Ok`,
  `Backpressured`, `NotFound`, `NotConnected`, `Terminated`, and
  `NotAdmitted` are returned as normal submit results, preserving the detail
  Core filled in. In particular, the non-zero detail of a `Backpressured`
  result where only some targets were accepted is not discarded.
  `InvalidArgument`, `InvalidHandle`, `InvalidState`, `NotSupported`,
  `ThreadViolation`, `OutOfMemory`, `SeqExhausted`, and `InternalError` are
  programming or system errors, so they raise `SubmitError`.
- Once a `publishAsync(...)` call has been queued, calling `close()` on the
  publisher rejects any new publish immediately. `close()` does not make the
  Node.js event loop wait. An operation that has already been queued or has
  started its Core call keeps the native publisher handle, and the native
  handle is released only after the last operation's Core call and Promise
  completion processing finish.
- `sendActorBoundSession(...)`, which sends from a MeshNode to an Actor's
  bound session, requires an `expectedBindingGeneration` greater than zero.
  It does not forward a call from a stale generation to the new session
  after the binding is replaced, and zero does not auto-select the current
  binding — it preserves Core's `InvalidArgument` result.

## Public Entry Shape

The package entrypoint groups the API around domain concepts.

- Core: context, version/capability lookup helpers, options, and utility
  functions.
- Messaging: `Message`, routing id values, received metadata, topic
  messages, subscription events, and stream packet data.
- Sockets: pair, dealer, router, pub, sub, xpub, xsub, stream, typed
  options, callbacks, request/reply, publish/subscribe, and stream packet
  APIs.
- Eventing: monitor, monitor snapshot/event, poller, poll events, and timer.
- Service: SPOT node, SPOT handle, topology snapshot, Actor reference, Actor
  lifecycle, and operation builders.
- Errors: typed error classes, or tagged error objects that preserve the
  core result domain.

## 64-bit Byte HWM and Monitoring Contract

Because the HWM and Auto HWM planning unit must represent a `uint64_t` byte
value losslessly, the public TypeScript type uses `bigint`. It does not also
accept `number` or change representation based on the safe-integer range.
`0n` means unlimited for an HWM, and the manual HWM default is
`4_096_000n` bytes. A negative value or a value above `2n ** 64n - 1n` is
rejected with `RangeError`; `number` and other types are rejected with
`TypeError`.

```ts
interface ContextOptions {
  autoHwmMsgUnitBytes: bigint; // 64-bit planning-unit bytes; 0n selects the socket default.
}

interface CommonSocketOptions {
  sendHwm: bigint; // Directional send-pipe byte HWM; 0n means unlimited.
  recvHwm: bigint; // Directional receive-pipe byte HWM; 0n means unlimited.
}
```

The monitor snapshot projects Core monitoring ABI v2 as-is. Planned,
applied, and deferred values, and in-flight HWM values, include `Bytes` in
their name and are provided as `bigint`. Whether a deferred value is valid
is provided as a separate boolean. Pending-message and profile-slot values
are count diagnostics and do not share a name with a byte field. An old
count-based name such as `autoHwmAppliedSndHwm` is not kept as an alias.

## Required Capability Coverage

Once aligned to the shared .NET-standard policy, the public entrypoint
covers all of the following stable user-facing capabilities.

- Context lifecycle, options, shutdown, auto-HWM recalculation, version,
  capability lookup, and strerror.
- Message ownership, multipart payload, routing id, received metadata,
  topic message, subscription event, and stream packet callback.
- Every socket family and its typed options.
- Monitor, poller, timer, and readiness semantics.
- SPOT node, SPOT handle, topology snapshot, Actor, and stream Actor
  binding.

The binding may expose a synchronous or asynchronous form where
appropriate, but it does not change the meaning of a core operation.

## Spot Get-Or-Create

Node exposes `SpotNode.getOrCreateSpot(spotRid)`. This maps directly to
`zlink_spot_node_spot_get_or_new(...)`; it is not implemented by combining
`spotLookup` and `createSpot`.

This method returns `{ spot, created }`. The returned `Spot` is caller-owned
and is closed the normal way. `created` is `true` only for the call that
created the logical spot.

## Receive and Subscribe Shape

- The data-plane receive and subscribe APIs use a caller-provided result
  object for reusable storage.
- Non-blocking no-data returns `false`, distinct from a thrown error.
- A SPOT readable dispatch event is a readiness notification. The caller
  drains the matching receive API until it reaches no-data.
- Native-backed buffers become owned `Message` objects without an
  additional JavaScript buffer concatenation.
- A service control/admission receive path such as Actor join request
  receive can use a nullable, `undefined`, or tagged result-return shape
  when that is clearer than reusable data-plane storage. It still
  distinguishes no-data from a thrown hard receive error.

## Error and Validation Policy

- Validate fixed-size boundary strings and ids before the native addon
  call.
- Do not silently truncate a routing id, actor id, endpoint, channel name,
  or topic.
- Preserve the submit, request, recv, handler, close, bind, connect, and
  config error domains.
- A public error carries enough structured data for a caller to branch
  without parsing error text.

## Performance Policy

- A hot path does not use reflection-style property walking, dynamic
  dispatch by string lookup, avoidable allocation, avoidable `Buffer`
  copies, hidden sleeps, busy waits, broad locks, or worker-thread joins.
- Request progress is shared per native handle while a request is
  outstanding.
- Poll result materialization uses a fixed mapping table, not per-event
  reflective enum scanning.
- Perf, samples, and tests import only the public package entrypoint.

## Implementation Checklist

- `package.json` exports do not expose a private module.
- The published `.d.ts` file describes the public contract.
- Native addon detail does not leak through a public type.
- An exposed helper function or builder convenience method is declared in
  contract source, not only in a runtime helper.
- Receive/subscription semantics match the shared binding policy.
- Wherever a service control/admission receive differs from caller-provided
  data-plane storage, that exception is documented.
- Perf semantics match `bindings/c/perf`.
- `src/zlink/contracts` has no import or export dependency on
  `src/zlink/runtime`.
- `src/index.ts` imports a runtime module only for factory wiring, and does
  not export a runtime module or runtime implementation type name.
- Tests, samples, and perf do not use a deep runtime import.
- A native-backed resource is created through a package-root factory or
  contract method, and is typed as a contract interface.
- Every native-backed resource, operation builder, and callback role listed
  in Library Shape has its public contract interface first, before the
  runtime implementation class is wired into a factory.
- No old alias, duplicate operation-start name, or deprecated wrapper is
  kept only for compatibility.

Required verification after the Node refactor. Run the following commands
from `bindings/node/`.

- Run `npm run build`.
- Run `npm run typecheck`.
- Run `npm test`.
- Run `npm run samples` if a public example or construction path changed.
- Run `npm run perf:single` and `npm run perf:multi` as smoke gates if a
  hot path, receive, send, request, poller, timer, or service behavior
  changed.
- Inspect the generated declarations and confirm the package root exposes
  contract types, not runtime implementation modules.
- Search the public surface for a private import. At minimum, check
  `src/zlink/contracts`, `tests`, `samples`, and `perf` for an import from
  `src/zlink/runtime`, a runtime handle aggregate, a native addon module, or
  a generated private file. Check `src/index.ts` separately to confirm its
  runtime import is factory-wiring only and does not appear in an exported
  declaration.

## Actor and Spot Route Results

Node exposes Actor and Spot route lookup results as public JavaScript
objects with matching TypeScript declarations.

- `ActorRoute` preserves the resolved Actor reference, Actor node RID,
  current Spot RID, and current Spot kind.
- `SpotRoute` preserves Spot RID, owner node RID, and Spot kind.
- `SpotKind` distinguishes Entry Spot from a user Spot. An invalid kind is
  not a successful route result.
- A SpotNode snapshot entry exposes the same Spot kind/current Spot fields
  as the core snapshot.

- Node exposes `SpotNode.sendToActor(actorRef)` and
  `SpotNode.requestToActor(actorRef)`, taking a resolved Actor ref as their
  argument.
- The send operation hands off ownership of one or more message parts once
  submit succeeds, and completes once the Actor owner mailbox accepts the
  handoff.
- The request operation hands off ownership of the request part once submit
  succeeds, and delivers the reply part the Actor handler produced.
- Node does not revive a removed Discovery route table or resolver API as a
  compatibility helper.
