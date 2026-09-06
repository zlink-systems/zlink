---
title: "C Bindings Implementation Blueprint"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: Async & Coroutine Policy](../async-coroutine-policy.en.md) | [Next: .NET](../dotnet/README.en.md)
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
| [Byte HWM and Auto-HWM](#byte-hwm-and-auto-hwm) | Core ABI byte-HWM configuration, calculation, and admission rules |
| [Receive flow state](#receive-flow-state) | The receive-flow state enum, function, results, and monitor surface |
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

## Byte HWM and Auto-HWM

In C, the Core ABI provides the [HWM](../../../../core/doc/spec/core/glossary.en.md#hwm) (the queue byte threshold) contract directly. `ZLINK_OPT_SNDHWM`
and `ZLINK_OPT_RCVHWM` are `uint64_t` accounted-byte limits, and
`zlink_set_option()` and `zlink_get_option()` receive exactly 8 bytes of
storage. The manual default is `4,096,000 bytes`, and `0` means unlimited.

The context memory limit and Core budget use byte-valued `uint64_t` options; the profile uses
its canonical option. Planning, manual overrides, and admission follow
[Core HWM calculation and admission](../README.en.md#hwm-calculation-and-admission).

In `zlink_monitor_status_t` ABI version 4, planned, applied, and deferred HWM
values and in-flight usage use bytes. `snd_pending_msgs` and
`rcv_pending_msgs` are display counts, not admission inputs. No message-unit,
slot, size-cap, or connection-bucket diagnostic is exposed.

`zlink_socket_monitor_open_options_t.monitor_hwm_bytes` is the only monitor
queue HWM option. Zero selects the Core default; a positive value is forwarded
unchanged as the exact byte limit. No message-count alias or conversion is
provided.

## Receive flow state

C exposes `zlink_receive_flow_state_t` and the following Core function unchanged.

```c
typedef enum zlink_receive_flow_state_t
{
    ZLINK_RECEIVE_FLOW_RUNNING = 0,
    ZLINK_RECEIVE_FLOW_PAUSED = 1
} zlink_receive_flow_state_t;

ZLINK_EXPORT zlink_config_result_t zlink_socket_set_receive_flow_state (
  void *handle_, zlink_receive_flow_state_t state_);
```

The return value is `zlink_config_result_t`; `zlink_errno()` provides the detailed error.
State, result, and monitor projection follow the [common receive-flow contract](../README.en.md#receive-flow-projection).
The [Core monitoring ABI](../../../../core/doc/spec/core/06-monitoring.en.md#61-abi-version-and-layout)
owns the C enum, event, flag, and status-field declarations.

## Required feature coverage

A C review checks the following groups in `core/include/zlink.h`.

- Runtime, version, capability lookup, context lifecycle, context options.
- Message lifecycle, message data access, copy/move/adopt rules, attribute lookup.
- Socket lifecycle, bind/connect, disconnect, options, TLS helpers, routing id, send, receive, request, reply, publish, subscribe, stream API.
- Eventing API: monitor, poller, timer, pull receive, and readiness semantics.
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
- Pull receive and poller use do not require the caller to know private worker, socket, or inproc endpoint details.

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

## Pull completion and STREAM packets

The C header and library ABI version follows [Core release metadata](../../../../VERSION).

C exposes REQUEST and WRITABLE through a `zlink_completion_t` output.
[Core completion pull and ownership](../../../../core/doc/spec/core/socket/README.en.md#completion-pull-and-ownership)
owns the receive and cleanup functions, readiness, and single drain owner.
SEND/REQUEST results, IDs, wait tokens, input retention, and resubmission conditions follow
[Core part send](../../../../core/doc/spec/core/socket/README.en.md#part-send-and-pending-admission) and
[Core request](../../../../core/doc/spec/core/socket/README.en.md#request-and-reply).
C adds no language terminal or completion registry.

A ROUTER REQUEST receive returns a nonzero `zlink_reply_token_t`. The token is an opaque capability
scoped to the responder ROUTER socket and source logical RID. A DATA token is `0`. After receiving every
part of a REQUEST, the application passes the source RID and token unchanged to reply.

STREAM selects `RAW` or `PACKET` receive mode before its first successful bind/connect. In `PACKET`
mode, `zlink_stream_recv_packet()` moves owned messages into caller-prepared empty header/body outputs
and returns the source RID as a borrowed view that remains valid until the next data-receive entry.

### Public interface

```c
typedef uint64_t zlink_completion_id_t;
typedef uint64_t zlink_reply_token_t;

typedef enum zlink_completion_kind_t {
  ZLINK_COMPLETION_SEND = 1,
  ZLINK_COMPLETION_REQUEST = 2,
  ZLINK_COMPLETION_WRITABLE = 3
} zlink_completion_kind_t;

typedef enum zlink_send_complete_result_t {
  ZLINK_SEND_ADMITTED = 0,
  ZLINK_SEND_TERMINAL = 202
} zlink_send_complete_result_t;

typedef struct zlink_completion_t {
  uint32_t struct_size;
  zlink_completion_kind_t kind;
  zlink_completion_id_t completion_id;
  void *user_context;
  zlink_routing_id_t peer_rid;
  zlink_send_complete_result_t send_result;
  int send_terminal_errno;
  zlink_request_result_t request_result;
  zlink_msg_t *reply_parts;
  size_t reply_part_count;
} zlink_completion_t;

ZLINK_EXPORT zlink_submit_result_t zlink_send_part(
  void *s_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid(
  void *s_,
  const zlink_routing_id_t *target_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_request_part(
  void *s_,
  const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_reply_part(
  void *router_,
  const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_recv_result_t zlink_completion_recv(
  void *s_,
  zlink_completion_t *completion_out_,
  zlink_recv_flags_t flags_);

ZLINK_EXPORT void zlink_completion_close(
  zlink_completion_t *completion_);

ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part(
  void *router_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_reply_token_t *reply_token_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);

typedef enum zlink_stream_recv_mode_t {
  ZLINK_STREAM_RECV_MODE_UNSPECIFIED = 0,
  ZLINK_STREAM_RECV_MODE_RAW = 1,
  ZLINK_STREAM_RECV_MODE_PACKET = 2
} zlink_stream_recv_mode_t;

typedef enum zlink_stream_option_t {
  ZLINK_STREAM_OPT_NOTIFY = 0x3501,
  ZLINK_STREAM_OPT_RECV_MODE = 0x3502
} zlink_stream_option_t;

ZLINK_EXPORT zlink_recv_result_t zlink_stream_recv_packet(
  void *stream_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *header_out_,
  zlink_msg_t *body_out_,
  zlink_recv_flags_t flags_);
```

The caller zero-initializes a `zlink_completion_t` output and sets `struct_size`. After a successful
recv, the caller closes SEND records as well with `zlink_completion_close()`. For a REQUEST `OK`
payload, `reply_parts` is a Core allocator base; the caller does not free the array directly. Close
releases remaining messages and the array, then restores an empty aggregate while preserving
`struct_size`.

Only the following names and values appear in the public enum for pending options.

```c
ZLINK_OPT_PENDING_MAX_MSGS  = 0x303A,
ZLINK_OPT_PENDING_MAX_BYTES = 0x303B
```

The public C ABI contains no `zlink_send_async*`, `zlink_send_complete_handler`,
`zlink_send_complete_handler_fn`, send/request/recv/STREAM/monitor/timer callback types or registration
functions, dealer/router-specific request/reply or exact-pair APIs, or `_v2` recv. Pending options also
contain no `SEND_PENDING` name.

Monitor and timer use a pull lifecycle. Monitor provides `zlink_socket_monitor_open()`,
`zlink_socket_monitor_recv()`, `zlink_monitor_status()`, and `zlink_monitor_close()`. Timer provides
`zlink_timer_new()`, `zlink_timer_start()`, `zlink_timer_stop()`, `zlink_timer_recv()`, and
`zlink_timer_destroy()`. Monitor `connection_id` is used only for diagnostics and correlation, not as a
send/reply target or reconnect fence.

## Implementation and contract-test verification requirements

Verify the following using only the public C ABI, return values, errno, and poller events. Each item maps
to one contract test.

**Submit and completion**

Submit results, IDs, and REQUEST/WRITABLE observations follow the
[Core submit/completion verification requirements](../../../../core/doc/spec/core/socket/README.en.md#8-implementation-and-contract-test-verification-requirements).
- `ZLINK_POLLCOMPLETION` does not consume a record in wait. Once a DONTWAIT drain empties the queue,
  recv returns `ZLINK_RECV_NO_DATA` with `EAGAIN`.
- Closing an output after successful completion recv restores an empty aggregate that preserves
  `struct_size`, and the same output can be reused.

**Reply token and STREAM**

- ROUTER DATA recv returns token `0`; every multipart part of a REQUEST returns the same nonzero token.
- Starting a reply with a token from another responder socket or source RID makes native submit fail.
- STREAM bind/connect fails while mode is `UNSPECIFIED`; after setting `RAW` or `PACKET`, only the
  corresponding recv family succeeds.
- Successful PACKET recv returns RID, header, and body. `NO_DATA` and errors leave the caller's empty
  outputs unchanged.

**Pull eventing**

- Monitor and timer recv return ready events and fire counts through pull and distinguish DONTWAIT
  no-data in their respective recv results.
