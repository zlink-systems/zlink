---
title: "C++ Bindings Final Structure"
---

<!-- bindings-nav:start -->
[Spec index](../README.md) | [Previous: .NET](../dotnet/README.md) | [Next: Java](../java/README.md)
<!-- bindings-nav:end -->

# C++ Bindings Final Structure

> **What this chapter defines** — the `Contracts`/`Runtime` shape the C++
> library must have once the binding refactor is done, and its required
> semantic scope.

This document defines the shape the C++ library must have once the
binding refactor is done. It does not enumerate every method. The concrete
public contract lives at `bindings/cpp/include/zlink/Contracts/`.

In the finished structure, `Contracts/`, the installed header projection,
tests, samples, the perf runner, and runtime behavior all map
`core/include/zlink.h`'s stable core features onto idiomatic C++ types.

This README defines how the C++ binding, in its final state, interprets
the common policy in `../README.md`. The finished C++ binding does not
keep old include paths, alias methods, alternate projections, or wrapper
layers meant to preserve a past surface. This document is the acceptance
criteria for the refactored C++ binding.

This binding follows the common bindings architecture map using C++
naming. `Contracts/` owns the installed public headers, and `Runtime/`
owns the private implementation under `src/`. The folder names organize
the repository — they are not a namespace segmentation users are meant to
depend on.

| Section | Covers |
|---|---|
| [Public contract source](#public-contract-source) | Contracts/Runtime source locations, the language baseline, the async surface policy link |
| [Repository layout](#repository-layout) | The aligned directory tree and the file-granularity policy |
| [.NET contract category projection](#net-contract-category-projection) | How the .NET layout is projected onto C++ naming |
| [Public contract at a glance](#public-contract-at-a-glance) | The area → public object → owning header table, and a concrete facade example |
| [Core feature ownership rule](#core-feature-ownership-rule) | The procedure to follow when adding a new feature |
| [Library shape](#library-shape) | RAII, snake_case, Pimpl, mandatory-builder rules |
| [Contract/runtime placement rules](#contractruntime-placement-rules) | The boundary between public declarations and runtime helpers |
| [Build and packaging policy](#build-and-packaging-policy) | The build/link/install rules for the `zlink_cpp` target |
| [Contract folder layout](#contract-folder-layout) | The ownership scope of each category under `Contracts/` |
| [Standard interface rules](#standard-interface-rules) | recv signatures, builders, handler naming rules |
| [64-bit byte HWM and the monitoring contract](#64-bit-byte-hwm-and-the-monitoring-contract) | The `byte_count_t` representation and the monitor snapshot fields |
| [Feature scope](#feature-scope) | The groups the finished public headers cover |
| [Lifetime and ownership](#lifetime-and-ownership) | Resource-class release/move rules and the receive-storage rules |
| [Error and result policy](#error-and-result-policy) | Failure representation and the result domain |
| [Performance policy](#performance-policy) | Hot-path and link-target constraints |
| [Finished structure requirements](#finished-structure-requirements) | What to confirm before declaring the structure finished |
| [Actor and Spot route results](#actor-and-spot-route-results) | The route-result types and Actor-directed send/request |

## Public contract source

- Public contract: `bindings/cpp/include/zlink/Contracts/`.
- Runtime implementation: `bindings/cpp/src/Runtime/`.
- Public entry point projection: `bindings/cpp/include/zlink.hpp`.
- Installed projection: `bindings/cpp/include/zlink.hpp` and `bindings/cpp/include/zlink/Contracts/...`.
- Compiled library: the C++ binding builds and installs a C++ library target such as `zlink_cpp`, separate from the core native `zlink` library.
- Language baseline: C++20.
- Namespace: every public type lives under `zlink`. Service types live under `zlink::service`.
- Internal implementation: native bridge helpers, callback trampolines, request-progress helpers, private `detail` helpers, private implementation headers, and `.cpp` files live under `bindings/cpp/src/Runtime/`.
- Documentation's role: this README defines the shape, boundaries, and required semantic scope. `Contracts/` owns the exact member list, and the installed headers intentionally project it.

- C++ is no longer modeled as a header-only binding.
- A second `bindings/cpp/src/zlink/Contracts/` tree is never created.
- The public contract stays in the installed headers, and the implementation moves behind `.cpp` files and private runtime headers. The contract/runtime separation still holds.
- `Contracts/` declares the user-facing surface, and `src/Runtime/` holds the implementation support for that surface.
- Java's or .NET's interface-centric layout is never copied onto C++ as-is.
- C++ uses installed headers, RAII classes, concrete values, and opaque implementation state as its natural boundaries.

- C++20 is the bindings library's minimum supported baseline.
- The bindings library may provide an `async_result_t<T>`-based completion object and callback submit, but it does not own a coroutine awaiter, a framework handler executor, or a framework dispatcher.
- A framework coroutine wraps bindings' completion object or callback completion at the framework's execution boundary.
- The per-language async execution surface baseline follows the [bindings async execution surface policy](../async-coroutine-policy.md).

## Repository layout

The finished C++ binding uses the following paths consistently. The file
names below are the target ownership map — a category is split further
only when a public concept or runtime responsibility has an independent
reason to change.

```text
bindings/cpp/
+-- CMakeLists.txt
+-- include/
|   +-- zlink.hpp
|   +-- zlink/
|       +-- Contracts/
|       |   +-- Core/
|       |   |   +-- capability.hpp
|       |   |   +-- context.hpp
|       |   |   +-- context_options.hpp
|       |   |   +-- routing_id.hpp
|       |   |   +-- utilities.hpp
|       |   +-- Messaging/
|       |   |   +-- message.hpp
|       |   |   +-- received.hpp
|       |   |   +-- topic_message.hpp
|       |   |   +-- subscription_event.hpp
|       |   |   +-- operation_contracts.hpp
|       |   |   +-- request_result.hpp
|       |   +-- Sockets/
|       |   |   +-- socket_contracts.hpp
|       |   |   +-- message_socket_contracts.hpp
|       |   |   +-- routed_socket_contracts.hpp
|       |   |   +-- pubsub_socket_contracts.hpp
|       |   |   +-- stream_socket.hpp
|       |   |   +-- socket_options.hpp
|       |   |   +-- results.hpp
|       |   +-- Eventing/
|       |   |   +-- monitor.hpp
|       |   |   +-- poller.hpp
|       |   |   +-- poll_event.hpp
|       |   |   +-- timers.hpp
|       |   |   +-- events.hpp
|       |   |   +-- status.hpp
|       |   +-- Service/
|       |   |   +-- spot_node.hpp
|       |   |   +-- spot.hpp
|       |   |   +-- actor.hpp
|       |   |   +-- spot_node_models.hpp
|       |   |   +-- actor_models.hpp
|       |   |   +-- operation_contracts.hpp
|       |   +-- Errors/
|       |       +-- errors.hpp
|       |       +-- results.hpp
+-- src/
|   +-- Runtime/
|       +-- zlink_cpp.cpp
|       +-- Core/
|       |   +-- capability.cpp
|       |   +-- context.cpp
|       |   +-- utilities.cpp
|       |   +-- operation_detail.hpp
|       |   +-- runtime_helpers.hpp
|       |   +-- types_impl.hpp
|       +-- Messaging/
|       |   +-- message.cpp
|       +-- Errors/
|       |   +-- error.cpp
|       +-- Eventing/
|       |   +-- monitor.cpp
|       |   +-- poller.cpp
|       |   +-- timers.cpp
|       +-- Sockets/
|       |   +-- base_socket.cpp
|       |   +-- pair.cpp
|       |   +-- dealer.cpp
|       |   +-- pubsub.cpp
|       |   +-- router.cpp
|       |   +-- stream.cpp
|       |   +-- detail.hpp
|       +-- Options/
|       |   +-- socket_options.cpp
|       +-- Service/
|       |   +-- actor.cpp
|       |   +-- actor_ops.cpp
|       |   +-- detail.hpp
|       |   +-- request_reply.cpp
|       |   +-- spot.cpp
|       |   +-- spot_node.cpp
|       |   +-- actor_detail.hpp
|       |   +-- spot_state.hpp
|       |   +-- spot_submit.hpp
|       +-- Native/
|           +-- socket_handle.hpp
|           +-- native_message_parts.hpp
|           +-- native_parts.hpp
|           +-- native_options.hpp
|           +-- native_send_result.hpp
+-- native/
+-- samples/
+-- tests/
+-- perf/
```

`CMakeLists.txt` defines the compiled C++ binding target (e.g.,
`zlink_cpp`) and links it against the core native `zlink` library.
Samples, tests, perf binaries, and applications link against this target
instead of compiling the private runtime source directly.

`Contracts/` is the public contract surface installed under
`bindings/cpp/include/zlink/`. `Runtime/` is the private implementation
support under `bindings/cpp/src/Runtime/`. The `zlink` namespace and
`zlink.hpp` are that contract projected onto C++. `Contracts` and
`Runtime` are never exposed as namespace segments.

A runtime helper header is not public contract API. Public samples, perf,
and tests include `<zlink.hpp>` and link against the C++ binding library —
they never include a runtime helper path. Wrapper headers such as
`include/zlink/message.hpp`, `include/zlink/services/spot.hpp`, and
`include/zlink/sockets/dealer.hpp` are not part of the finished layout.
The finished tree does not replace them with forwarding headers either.

Monitor, poller, and timer contracts live under the common `Eventing/`
category. `Contracts/Monitoring/` is not part of the finished public
contract, and the finished tree keeps no `Monitoring/` forwarding header.

File-level granularity follows the common policy in `../README.md`. Keep
one file per independent public concept, or per tightly coupled
operation/model group. Merge a very small marker, delegate, enum, or
pass-through helper file into the nearest contract file when doing so
makes the public shape easier to read.

## .NET contract category projection

The C++ binding uses the `.NET` public contract category layout as its
classification standard. This is a projection of categories and
responsibilities, not a copy of the C# shape. C++ keeps C++20 naming
conventions, headers, RAII facades, move semantics, and concrete value
types.

`.NET`'s file list is not copied into this document as-is. `.NET`'s single
baseline is the [.NET bindings blueprint](../dotnet/README.md),
specifically its Contract Folder Layout and Runtime Folder Layout
sections. This C++ README defines only the C++ projection of those
categories.

Category ownership rules are strict. A public C++ type does not move to a
different category just because its runtime implementation would be
easier to place elsewhere. Runtime helper code may be further subdivided
under `src/Runtime/`, but the public contract owner stays in its category.

- This projection does not strictly follow C#'s interface style.
- `.NET`'s socket role interfaces identify a socket role in the public contract. C++ is not required to expose `isocket_t`, `istream_socket_t`, `ISocket`, `IStreamSocket` by default.
- Use a concrete RAII facade unless the caller genuinely needs substitutable behavior.
- When a substitutable role is needed, keep the interface narrow, and make sure the send/receive/poll/dispatch hot path has no avoidable virtual dispatch.

## Public contract at a glance

The finished C++ binding makes the public contract visible without adding
an interface-only layer. A user starts at `<zlink.hpp>` and uses the
following map to find the owning contract header. The .NET baseline's
source-of-truth detail lives in the [.NET bindings blueprint](../dotnet/README.md); this document keeps only the C++ projection.

| Area | Public objects and role | Owning contract header |
|------|-------------------|----------------|
| Core | `context_t`, context options, routing id, version/capability helpers | `Contracts/Core/` |
| Messaging | `message_t`, `received_t`, `topic_message_t`, `subscription_event_t`, multipart helpers | `Contracts/Messaging/` |
| Sockets | `pair_socket_t`, `dealer_socket_t`, `router_socket_t`, `pub_socket_t`, `sub_socket_t`, `xpub_socket_t`, `xsub_socket_t`, `stream_socket_t`, send/recv/request/reply builders | `Contracts/Sockets/` |
| Eventing | `socket_monitor_t`, monitor events, poller, poll events, timer, readiness helpers | `Contracts/Eventing/` |
| Service | `spot_node_t`, `spot_t`, `actor_ref_t`, the actor lifecycle model, service operation builders | `Contracts/Service/` |
| Errors | Public exception and result domain types | `Contracts/Errors/` |

The map above is a public API index. It is the C++ equivalent of a
contract-surface overview, and does not imply abstract interfaces such as
`IContext`, `ISpot`, or `IActor`. A public resource object stays a concrete
RAII facade unless the caller genuinely needs substitutable behavior. A
narrow interface is allowed only for a role the user naturally swaps, such
as codec, callback, handler, or a poll target.

The public contract is read in two steps.

1. Start at `<zlink.hpp>` to see which public contract categories the C++ binding includes.
2. Open the matching `Contracts/...` header to look at the concrete public type and its public member list.

For example, the finished SPOT surface looks like a concrete facade rather
than an interface/implementation pair.

```cpp
namespace zlink::service {

class spot_t {
public:
    spot_t(spot_t&&) noexcept = default;
    spot_t(const spot_t&) = delete;

    send_operation_t send();
    reply_operation_t reply();
    int recv(received_t& out, recv_flags_t flags = recv_flags_t::none);
    void set_send_ready_handler(std::function<void()> handler);
    void close();
};

} // namespace zlink::service
```

`zlink.hpp` acts as the public table of contents for these facades.

```cpp
#include "zlink/Contracts/Core/capability.hpp"
#include "zlink/Contracts/Core/context.hpp"
#include "zlink/Contracts/Core/context_options.hpp"
#include "zlink/Contracts/Core/routing_id.hpp"
#include "zlink/Contracts/Messaging/message.hpp"
#include "zlink/Contracts/Messaging/received.hpp"
#include "zlink/Contracts/Messaging/topic_message.hpp"
#include "zlink/Contracts/Messaging/subscription_event.hpp"
#include "zlink/Contracts/Messaging/operation_contracts.hpp"
#include "zlink/Contracts/Sockets/message_socket_contracts.hpp"
#include "zlink/Contracts/Sockets/routed_socket_contracts.hpp"
#include "zlink/Contracts/Sockets/pubsub_socket_contracts.hpp"
#include "zlink/Contracts/Eventing/poll_event.hpp"
#include "zlink/Contracts/Eventing/poller.hpp"
#include "zlink/Contracts/Service/spot_node.hpp"
#include "zlink/Contracts/Service/spot.hpp"
#include "zlink/Contracts/Service/actor.hpp"
#include "zlink/Contracts/Errors/errors.hpp"
```

Runtime detail lives behind the facade. A public header may name an
opaque implementation state, but never exposes a native handle, a
callback trampoline, the part loop, the request pump, or a marshalling
helper.

```cpp
namespace zlink::service {

class spot_t {
public:
    spot_t(spot_t&&) noexcept;
    spot_t(const spot_t&) = delete;
    ~spot_t();

    send_operation_t send();
    reply_operation_t reply();
    void close();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace zlink::service
```

This structure keeps the public surface easy to scan while preserving C++
ownership semantics. `spot_t`, `spot_node_t`, and `actor_ref_t` are the
contract; `src/Runtime/...` and the private `zlink::detail` helpers are
implementation support.

## Core feature ownership rule

Every stable core feature C++ exposes follows this ownership rule.

1. Add the public type or method to the correct `bindings/cpp/include/zlink/Contracts/` category.
2. Update `bindings/cpp/include/zlink.hpp` and any intentionally installed projection header.
3. Decide the C++ domain owner: one of context, message, socket, monitor, timer, service, SPOT, actor, error, option.
4. Put raw C handle access, the `*_part` loop, callback userdata, trampoline state, and native marshalling helpers in `src/Runtime/` headers and `.cpp` files.
5. Add a public header test and update at least one sample/perf case when the new feature affects a user workflow or a measurement.
6. Confirm the new public API isn't just a thin C wrapper. Keep it internal if it merely delegates without improving ownership, validation, or shape.
7. Never leave behind a public name that's fallen out of use. Do not keep an aliasing deprecated form, a forwarding overload, or an alternate public header, unless a later document explicitly changes this C++ policy.

For explicitly acquiring a Spot by routing id, the C++ binding exposes
`spot_node_t::get_or_create_spot(routing_id_t)`, which maps directly to
`zlink_spot_node_spot_get_or_new(...)`. This method returns an owned
`spot_t` facade and a creation flag. This behavior is never implemented by
combining `spot_lookup()` and `create_spot()`.

## Library shape

The C++ binding should feel like a small native C++ library layered on
the core C contract.

- A public resource object is an RAII class that owns or borrows a native handle, per its documented lifetime.
- The destructor releases the resource without requiring the caller to know the native close order. The resource destructor, and any other non-trivial method, is defined out-of-line in a `.cpp` file.
- A small value-type operation may stay inline when it does not expose native ownership, callback state, request state, or marshalling detail.
- Public methods use `snake_case`.
- Public value types — message, routing id, received metadata, topic message, result, error, enum, option — stay concrete.
- A public resource header uses an opaque implementation state, such as Pimpl, to keep native handle layout, callback state, request state, and ABI-sensitive storage from leaking into the contract.
- Templates, overloads, and move semantics are used only to simplify caller ownership or avoid a copy — never to expose template machinery as a substitute for a clear domain type.
- A virtual interface is used only when the caller needs substitutable behavior. Not every handle is wrapped in an abstract interface by default.
- A builder is required for multipart send, publish, request, reply, actor, and SPOT operations — this hides native request state and makes ownership clear.

## Contract/runtime placement rules

- Public declarations and user-visible behavior live in `Contracts/`.
- A public free function, static helper, extension-style helper, or builder convenience helper lives in `Contracts/` when the caller can call it directly.
- Runtime handle owners, the socket kernel, the request pump, callback trampolines, and part-loop helpers live in `src/Runtime/`.
- FFI declarations, raw C handles, native struct mirrors, marshalling helpers, and platform loading code live in `src/Runtime/Native/`.
- `zlink.hpp` projects `Contracts/`. A `Runtime/` helper path is never given a public-include style.
- A contract header never includes a private runtime header. If a public class needs implementation state, it exposes only an incomplete `impl` type or another opaque private member, and defines behavior in the `.cpp` file.
- A runtime concrete class is not a user entry point. If the behavior is public, `Contracts/` declares it and `Runtime/` implements it.

## Build and packaging policy

Once C++ moves off header-only, the binding carries one more compiled
artifact. The finished binding therefore keeps the following build rules.

- The C++ binding builds a `zlink_cpp` library target.
- `zlink_cpp` links against the core native `zlink` library and follows a version-compatibility rule with the core library.
- The Linux, macOS, and Windows packages build the C++ library for each supported architecture and runtime toolchain.
- CMake install/export metadata lets an application consume the public headers together with the compiled C++ binding target.
- Samples, tests, and the perf runner link against the same installed-style C++ target an application uses — they never depend on a private runtime source path.
- The runtime search path, DLL lookup rules, and packaged native artifacts are tested, since an application now loads the core native library and the C++ binding library together.
- Public headers avoid exposing ABI-sensitive implementation storage. Public method signatures may keep C++ idiom, but native handle layout, callback state, request state, and marshalling buffers stay outside the installed headers.

## Contract folder layout

`Contracts/` is the source ownership map for public C++ declarations.
`zlink.hpp` projects these categories into the `zlink` namespace.

- `Core/`: context, context options, routing id, utility resources, and public free functions such as version or capability helpers.
- `Messaging/`: message, received metadata, topic message, subscription event, stream packet callbacks, builder payload helpers. A codec helper is not included in the C++ binding package — framework-level serialization is handled by a framework codec extension.
- `Sockets/`: socket operations, socket family, typed options, request/reply, publish/subscribe surfaces.
- `Eventing/`: monitor, monitor snapshot/event, poller, poll event, timer, public poll helpers.
- `Service/`: SPOT node, SPOT handle, the topology model, actor ref, actor lifecycle, operation builders.
- `Errors/`: an exception or typed error-result domain.
- Enum, flag, and result types live in the category that defines their meaning. No `Enums/` folder is created just to group declarations by syntax.

## Standard interface rules

- Data-plane `recv`, routed recv, subscribe, and subscription-event receive use caller-provided output storage (e.g., `received_t&`, `topic_message_t&`, `subscription_event_t&`).
- `message_t::from(...)` copies the caller's bytes independently. When a caller-owned buffer must be handed to a message without a copy, use the advanced API overload `external_message_t::from(span, free_fn, hint)`. This overload entrusts the buffer to the message, and calls `free_fn(data, hint)` exactly once when the message releases the buffer.
- send, routed send, publish, request, reply, SPOT operations, and Actor location/session operations return a move-only fluent builder.
- A builder's start method takes only a target identity, topic, channel, routing id, or request sequence. Payload, flag, timeout, callback, and async submit choices happen at the builder stage.
- A SPOT channel-targeted operation uses `send_to_channel(...)` and `request_to_channel(...)`. SPOT topic publish keeps `publish(topic)` as-is.
- A handler registration method uses the `set_..._handler` name. For example, send readiness uses `set_send_ready_handler(...)`, raw STREAM packet handling uses `set_packet_handler(...)`, a monitor event uses `set_monitor_handler(...)`, and SPOT dispatch uses `set_dispatch_handler(...)`.
- An `on_...` name is not a public registration method in the finished C++ API — it is reserved for an internal or protected hook when needed.
- No single-payload shortcut overload is added under the same name as an operation's start method. `send(message)`, `send(routing_id, message)`, `publish(topic, message)`, `send_to_channel(channel, message)`, `send_to_spot(..., message)` are not public contract members. A caller uses `send(...).message(message).submit()`.
- A multipart payload accumulates via repeated `message(...)` calls. A `messages(...)` convenience method is allowed only when it delegates to the same builder contract and is declared in `Contracts/`.
- A Dealer socket does not expose protocol envelope helpers such as `request_frame(...)` or `reply(request_token, parts)`. Dealer can start a request with `request()`, but has no API-level peer routing id, so it cannot reply to an arbitrary token.
- No operation-start overload family such as `send_no_wait`, `publish_with_flags`, `request_async` is added. Keep one operation name, and let the builder absorb variants. The per-language name of the terminal builder method follows the [bindings async execution surface policy](../async-coroutine-policy.md).
- No standard-name bypass or operation alias such as `on_send_ready(...)`, `on_packet(...)`, `on_event(...)` is kept. A call site uses the standard public contract directly, not a layered alias.

## 64-bit byte HWM and the monitoring contract

- HWM and the Auto HWM planning unit are expressed as `byte_count_t`.
- This value type holds only `uint64_t` bytes, and reveals its unit through the `bytes(...)` constructor function and the `bytes()` accessor function.
- The old `message_count_t` is not kept as an alias or an adapter.
- `0` means unlimited for HWM, and the manual default is `4,096,000 bytes`.

```cpp
auto options = socket.options ();
options.send_hwm (zlink::byte_count_t::bytes (send_limit)); // Sets the send pipe's byte HWM.
options.recv_hwm (zlink::byte_count_t::bytes (0));          // 0 means unlimited receive HWM.

auto context_options = context.options ();
context_options.auto_hwm_msg_unit_bytes (
  zlink::byte_count_t::bytes (planning_unit)); // The 64-bit byte input for the Auto HWM calculation.
```

The monitor snapshot projects the Core monitoring ABI v2. The planned,
applied, deferred, and in-flight HWM fields use a `_bytes` suffix and
`uint64_t`. A deferred field's validity is provided as a separate boolean.
Pending message and profile slot values stay count diagnostics and never
share a name with a byte field.

## Feature scope

The finished C++ binding's public headers cover the following groups.

- Core: context, version/capability helpers, context options, shutdown, auto-HWM recalculation, `atomic_counter_t`, `stopwatch_t`, `thread_t`.
- Messaging: message ownership, builder multipart input, received metadata, topic message, subscription event, routing id, callback types.
- Socket family: pair, dealer, router, pub, sub, xpub, xsub, stream, stream-bound actor snapshot, common options, typed socket options, bind/connect/disconnect, TLS, callbacks, request/reply surfaces.
- Eventing: socket monitor, monitor event, monitor snapshot, poller, one-shot `poll(...)`, poll event, timer, readiness flags.
- Services: SPOT node, SPOT handle, topology snapshot, actor ref, actor lifecycle, actor operations.
- Errors: a typed exception or error-result surface that preserves the core result domain.

The C++ surface never exposes a raw native handle, a `*_part` loop,
callback userdata, an internal inproc endpoint, or a request pump object
as a public concept.

## Lifetime and ownership

A C++ caller never has to reason about cleaning up a C handle.

- A resource class releases its native handle in the destructor, and supports an explicit `close` or an equivalent lifecycle method when close can fail.
- A move-only resource class is preferred over shared ownership of a mutable handle.
- A message value supports an efficient move, and an explicit copy when a copy is requested.
- The data-plane receive and subscribe paths use caller-provided storage.
- A service control/admission receive path, such as Actor join request receive, may use an optional or a typed result return when that's clearer for a C++ caller — but it must still distinguish no-data from a hard receive failure.
- A callback keeps the native callback lifetime and the user callable's lifetime internally consistent.

## Error and result policy

A binding may use either exceptions or typed result objects, but the
public shape preserves the core meaning.

- No-data and transient backpressure are kept distinct from a hard failure.
- request, submit, recv, bind, connect, config, handler, and close failures preserve the result domain meaning.
- `pollout` is a send-recovery readiness signal, not an ordinary writable bit.
- The ROUTER/PUB defaults, SPOT HWM defaults, and SPOT dispatch worker semantics follow the core header.

## Performance policy

- A multipart value is created directly from the core part substrate.
- The hot path avoids unnecessary heap allocation, avoidable copies, reflection-like dynamic dispatch, hidden waits, sleeps, busy waits, broad locks, and joins.
- Perf and samples include only the installed public headers.
- Perf and samples link against the public C++ binding target — never against a private runtime object file or a helper source directory.
- C++ perf semantics match `bindings/c/perf`: the same pattern semantics, the same transport semantics, the same client-count policy, with no private fast path.

## Finished structure requirements

The finished C++ binding satisfies the following requirements.

- The installed headers expose every stable, user-facing core feature.
- The C++ binding builds and installs a compiled C++ library target in addition to the public headers.
- `Contracts/Eventing/` is the only public eventing category. `Contracts/Monitoring/` is gone, and `zlink.hpp` includes the Eventing header.
- The old wrapper include paths are gone. Applications, samples, perf, and tests include only `<zlink.hpp>` or an intentional `Contracts/...` header.
- The public headers and the compiled C++ binding target alone give an application, perf, sample, or framework adapter everything it needs.
- A user never needs a private helper header or a private runtime source path.
- A value type stays concrete unless an abstraction genuinely reduces complexity.
- The public API hides the native part loop, raw handles, and callback userdata.
- Handler registration uses the `set_..._handler` name, and no public `on_...` alias is kept.
- A public helper/free function and a builder convenience method are declared in `Contracts/`, not as a runtime helper.
- An exception where service control/admission receive differs from the data plane's caller-provided storage is documented.
- Perf tests use the same measurement semantics as C perf.

## Actor and Spot route results

C++ exposes Actor and Spot route lookup results as concrete contract
types.

- `actor_route_t` preserves the resolved Actor ref, `actor.node_rid`, `current_spot_rid`, `current_spot_kind`.
- `spot_route_t` preserves `spot_rid`, `owner_node_rid`, `spot_kind`.
- `spot_kind` distinguishes an Entry Spot from a user Spot. An invalid kind is not a successful route result.
- `spot_node_spot_entry_t` and `spot_node_actor_entry_t` expose the same Spot kind/current Spot fields as the core snapshot.

- C++ exposes `spot_node_t::send_to_actor(actor_ref_t)` and `spot_node_t::request_to_actor(actor_ref_t)`, which take a resolved Actor ref.
- `send_to_actor`, once submit succeeds, transfers ownership of one or more message parts, and completes once the Actor owner's mailbox takes them over.
- `request_to_actor`, once submit succeeds, transfers ownership of the request part and delivers the reply part the Actor handler produced, as a callback or an awaitable result.
- C++ must not resurrect the removed Discovery route table or resolver API as a compatibility helper.
