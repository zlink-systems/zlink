---
title: "C Bindings Implementation Blueprint"
---

<!-- bindings-nav:start -->
[Spec index](../README.md) | [Previous: Async & Coroutine Policy](../async-coroutine-policy.md) | [Next: .NET](../dotnet/README.md)
<!-- bindings-nav:end -->

# C Bindings Implementation Blueprint

> **What this chapter defines** — the shape the C binding must take on top of
> the `core/include/zlink.h` ABI, and its review rules.

This document defines the shape the C binding must take. It does not
duplicate every public function signature — the concrete public API
contract is `core/include/zlink.h`.

In C, the native ABI itself is the binding contract. `bindings/c` does not
add a second contract/runtime layer on top of the core C API. The C
implementation is considered aligned once the public header, native
library behavior, tests, samples, packaging, and perf runner all match this
document and the rules in `core/include/zlink.h`.

The shared bindings architecture map still holds as review vocabulary.
core, messaging, sockets, eventing, service, and errors are the conceptual
areas a reviewer uses when reading the C header. C expresses these areas
not as separate `contracts/` and `runtime/` folders, but through header
sections, type/function prefixes, tests, samples, and documentation
sections.

The shared file-granularity policy is only review vocabulary for C header
sections and helper files. Because `core/include/zlink.h` remains the ABI
baseline, it does not require wrapper-style `contracts/` or `runtime/`
folders.

| Section | Covers |
|---|---|
| [Public contract source](#public-contract-source) | The boundary between public ABI, internal implementation, and documentation's role |
| [Repository structure](#repository-structure) | The paths used when changing the C binding |
| [API change procedure](#api-change-procedure) | The order for adding or changing a feature |
| [Library shape](#library-shape) | Native ABI function naming, flag, and multipart rules |
| [Interface shape exceptions](#interface-shape-exceptions) | Where the higher-level binding wrapper rules don't apply to C |
| [Required feature coverage](#required-feature-coverage) | The header feature groups a review checks |
| [Spot Get-Or-New](#spot-get-or-new) | The `zlink_spot_node_spot_get_or_new` contract |
| [Ownership and lifecycle](#ownership-and-lifecycle) | Handle/message ownership transfer rules |
| [Error and Result policy](#error-and-result-policy) | The C result domain, and why no exceptions |
| [Performance policy](#performance-policy) | Why C is the performance baseline for the other bindings |
| [Implementation checklist](#implementation-checklist) | What to confirm before declaring alignment |
| [Actor and Spot route results](#actor-and-spot-route-results) | The route-result structs and the routing helper policy |

## Public contract source

- Public contract: `core/include/zlink.h`.
- Public ABI: the exported `zlink_*` functions, public structs, enums, constants, callback typedefs, and ownership rules that header declares.
- Internal implementation: files under `core/src/`, private helper headers, generated bridge files, build scripts, test helpers.
- Documentation's role: this README describes the C shape and review rules. The header owns the exact list of binding signatures.

No second C facade is introduced on top of `zlink.h`. A local alias function
that just forwards to another `zlink_*` function, an alternate option bag,
or a compatibility wrapper is not part of the binding contract.

## Repository structure

Use the following paths consistently when changing the C binding.

- Public contract: `core/include/zlink.h`.
- Runtime implementation: `core/src/`.
- Native artifacts: `core/build`.
- Bindings include projection: `bindings/c/include/` (when packaging needs installed headers).
- Tests: `bindings/c/tests/`.
- Samples: `bindings/c/samples/`.
- perf: `bindings/c/perf/`.
- C API mapping, sample/test support, and perf policy live under `bindings/c/`.

Temporary build directories and generated output are not contract
locations.

```text
zlink/
+-- bindings/
|   +-- c/
|   |   +-- include/
|   |   +-- tests/
|   |   +-- samples/
|   |   +-- perf/
+-- core/
|   +-- include/
|   |   +-- zlink.h
|   +-- src/
|   +-- build/
```

## API change procedure

When adding or changing a C feature:

1. Add or update the public declaration in `core/include/zlink.h`.
2. Implement the behavior under `core/src/`.
3. Update the errno/result documentation if the result domain changed.
4. Add a test that includes only the public header.
5. Update a sample only if the user-facing shape changed.
6. Update the perf runner only if the measured behavior changed.
7. Rebuild `core/build` before interpreting C perf results.

## Library shape

The C binding keeps the native ABI shape.

- Function names use `zlink_*` and `snake_case`.
- Blocking vs. non-blocking behavior is chosen with a flag such as `ZLINK_DONTWAIT`, not a separate public `try_*` function.
- The send path returns `zlink_submit_result_t` or a documented request result.
- The recv path returns `zlink_recv_result_t` and fills a caller-owned output storage per the header contract.
- Multipart payloads use repeated `zlink_msg_t *part` calls and `zlink_part_flag_t`.
- Routing APIs use an explicit routing id parameter and an explicit output routing id storage.
- A callback API exposes a C function pointer and userdata only when the public header declares it.

Higher-level object convenience forms such as `Received.Reply(...)`,
`Socket.Send().Message(...).Submit()`, and `Spot.Publish(topic)` do not
apply to C. Those shapes belong to the higher-level bindings.

## Interface shape exceptions

C is the ABI baseline and does not adopt the wrapper-binding interface
rules.

- Receive and subscribe use the output parameters `zlink.h` declares.
- Send, publish, request, and reply use explicit `zlink_*` functions and flags.
- C does not expose an operation builder, a staged interface, or a fluent helper object.
- The wrapper-binding rules for public static facades, builder convenience helpers, and contract/runtime folders do not apply to C. A public C helper must be declared in `core/include/zlink.h`; otherwise it is an internal helper.
- C samples and perf include the public header and call the public C ABI directly.

## Required feature coverage

A C review checks the following groups in `core/include/zlink.h`.

- Runtime, version, capability lookup, context lifecycle, context options.
- Message lifecycle, message data access, copy/move/adopt rules, attribute lookup.
- Socket lifecycle, bind/connect, disconnect, options, TLS helpers, routing id, send, receive, request, reply, publish, subscribe, stream API.
- Eventing API: monitor, poller, timer, callback registration, readiness semantics.
- SPOT node, SPOT handle, topology snapshot, actor, service-layer API.
- error/result enums and errno mapping.

If a feature exists in `core/include/zlink.h`, the C binding exposes it
directly through the public header. A feature not in that header is not
public C API.

## Spot Get-Or-New

`bindings/c/include/zlink.h` exposes `zlink_spot_node_spot_get_or_new(...)`
with the same signature and result contract as the core public header.
This function atomically gets or creates a local logical Spot by routing
id, and returns both a caller-owned `Spot` facade handle and a
was-it-created flag.

This API does not join an actor to the Spot — join remains a separate
service operation.

## Ownership and lifecycle

A C caller owns memory explicitly. The header makes ownership transfer
explicit at every boundary.

- A `zlink_msg_t` value is initialized before use and closed exactly once.
- An API that moves or adopts message storage documents the source object's state after the call.
- A recv API fills caller-provided storage. The caller closes any message part whose ownership was transferred to it.
- A handle is closed through the matching `zlink_*_close`, `zlink_*_destroy`, `zlink_ctx_term`, or a documented lifecycle function.
- Callback registration does not require the caller to know private worker, socket, or inproc endpoint details.

## Error and Result policy

The C binding reports public results as a C result domain, not exceptions.

- Transient no-data is reported as a documented recv result.
- Transient backpressure is reported as a documented submit result.
- Configuration, bind, connect, close, request, handler, and recv failures map to the result and errno rules in `zlink.h`.
- The public header does not require a caller to inspect private implementation state to classify a failure.

## Performance policy

The C binding is the performance baseline for the other bindings.

- The hot path does not add aggregate materialization when the public part substrate can stream parts as-is.
- The send/recv, request, dispatch, poller, timer, stream, SPOT, and actor paths do not add hidden sleeps, busy waits, thread joins, reflection-like dynamic dispatch, coarse global locks, or avoidable copies.
- The perf runner and samples include only the public header.
- `bindings/c/perf` measures `core/build`'s runtime unless the perf policy explicitly says otherwise.

## Implementation checklist

Before declaring the C binding aligned:

- `core/include/zlink.h` declares the exact public C ABI.
- The header documentation and the errno/result documentation agree.
- Public tests and samples compile without private headers.
- A higher-level binding can implement its own public shape without calling private C helpers.
- Perf output prints the runtime library path and does not run against a stale `core/build` runtime.

## Actor and Spot route results

The C binding exposes the core route-result structs directly as public
ABI.

- `zlink_actor_route_t` carries a resolved Actor ref. Besides `actor.node_rid`, it includes `current_spot_rid` and `current_spot_kind`.
- `zlink_spot_route_t` carries the requested `spot_rid`, `owner_node_rid`, and `spot_kind`.
- `zlink_spot_kind_t` distinguishes an Entry Spot from a user Spot. An invalid kind is not a successful Actor or Spot route result.
- A C sample that routes by Actor id first resolves the Actor, then passes `actor.node_rid` and `current_spot_rid` to the existing Spot routed API.

The C binding does not add `zlink_router_send_actor`,
`zlink_router_request_actor`, or an Actor-to-ROUTER request helper.
Actor-directed delivery is handled by route lookup followed by the
existing Spot routed send/request.
