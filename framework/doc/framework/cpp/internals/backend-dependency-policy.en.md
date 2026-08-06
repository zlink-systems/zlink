<!-- framework-adapter-nav:start -->
[Document List](../../../README.en.md) | [Previous: Runtime Architecture](../../common/internals/README.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:end -->

[C++ Bundle](../README.en.md) | [Public Interfaces](../../common/spec/server/languages/cpp/interfaces/README.en.md)

# ZLink Framework C++ Backend Dependency Policy

## 1. Purpose

Defines the backend boundary so the framework public API doesn't couple to the
zlink binding's socket objects or native implementation details.

## 2. Principles

- The framework calls only the binding's public API.
- Backend context, socket, SpotNode, Spot, and monitor handles stay in the
  private runtime.
- Public headers do not expose Boost.Asio, Boost.Beast, OpenSSL, or the
  binding's concrete socket types.
- Only `RoutingId`, messages, and explicitly approved transport value types
  stay in the public contract.
- Serializer, location codec, and wire-framing decisions are each owned by
  a single runtime subsystem.

## 3. Adapter Responsibility

The backend adapter handles context creation, socket bind/connect, frame
submit/receive, Spot and stream operations, and monitoring event
conversion. Registration, handler dispatch, timeouts, and application
lifecycle are handled by the framework runtime.

Samples and applications do not work around a missing backend capability
with raw frames, private headers, or test adapters. If a needed binding
public API doesn't exist, it's first designed and implemented as part of
the binding contract.

## 4. C++ Responsibility Graph

The C++ implementation follows the [common layering
principle](../../common/internals/01-layering.en.md). Public headers and the
domain runtime do not expose binding types. Where binding and framework
semantics are identical, the semantic runtime calls the binding public API
directly. Where ownership, lifecycle, readiness, error, or concurrency rules
change, a semantic adapter absorbs that difference.

| Path | Binding operation | Semantic difference and ownership | Decision |
|------|-------------------|-----------------------------------|----------|
| `raw_route_port_t`, `raw_dealer_port_t` | `socket_t::recv`, `send`, `request`, `reply`, `poller_t::wait` | Converts binding `received_t` and messages into framework routing id, request sequence, and raw-message results. The port owns socket and poller lifetime. | Keep semantic adapter |
| `raw_mesh_node_owner_t`, `raw_client_server_owner_t` | Context, routed socket, monitor, and poller | Combines several binding objects into one mesh or client/server connection lifetime and maps monitor results to framework state. | Keep semantic adapter |
| `raw_fanout_owner_t` | Pub/sub socket, `topic_message_t`, and poller | Separates reserved beacons from application topics and manages reconnect, readiness, and close ordering. | Keep semantic adapter |
| `channel_host_service_t`, `stream_host_service_t` | Binding socket and stream public operations | Converts binding results into channel dispatch or stream-session semantics. No additional wrapper forwards the same socket operation. | Call binding public API from semantic runtime |
| Store and relocation adapters | Provider/store operations | Converts storage records into framework location, ownership, and lifecycle results, including error and atomic-state rules. | Keep semantic adapter |
| Factory and transport selection | Context or transport creation | Composes and selects components; it is not a facade that only renames binding methods. | Keep composition boundary |

A backend or contract with no runtime caller and only one forwarding
implementation is not part of this graph. Keeping such a file in CMake makes
it look like a supported boundary while hiding no responsibility, so it is
removed.

## 5. Message Receive Path and Cost

The binding's `socket_t::receive` fills the `received_t` supplied by the
caller. The binding runtime reuses one native receive envelope per socket and
retains the existing message-vector capacity when assigning a new receive
result to `received_t`. Repeated receives therefore do not create a new
envelope and result vector for every message.

The framework's `raw_route_port_t`, `raw_dealer_port_t`, channel receive loop,
and fanout subscriber loop also retain one `received_t` or `topic_message_t`
per execution owner for the next receive. When a result must cross an
asynchronous handler boundary, the loop copies the framework messages needed
by the handler once and closes the binding receive object immediately. This
copy is the ownership boundary that prevents the binding object from being
used after the loop advances; it does not move codec or raw-frame work into
the caller.

The poller fills caller-provided `poll_event_t` storage, and framework poll
loops reuse that storage on the stack. They do not create a wrapper collection
for each poll result. The socket mutex remains the existing boundary that
protects a binding socket's execution owner; no general-purpose lock is added
around the same operation.

## 6. Alternative Review Rule

When connecting a new binding operation, the following alternatives are
reviewed first.

1. Call the binding public API directly from the semantic runtime when
   ownership, completion, error, and concurrency semantics match the
   framework contract.
2. Keep a semantic adapter or port only when several binding objects must be
   composed into one operation, a raw binding result must become a framework
   domain result, or lifecycle, readiness, or error responsibility must be
   changed to the framework rule.

Neither alternative accesses binding private members, reflection, visibility
hacks, or raw-frame workarounds. An `IBackend` that only wraps one
implementation is not added solely for testability.

## 7. Regression Tests

| Test Case | Pass Criteria |
|---------------|-----------|
| `test_cpp_framework_layout_contract` | Public headers expose no private runtime or backend concrete dependency. |
| `test_cpp_framework_contract_headers` | Public headers compile with no private include path. |
| `test_cpp_framework_raw_route_port_contract` | The raw route adapter maps routing id, request sequence, receive results, and failures to framework semantics. |
| `test_cpp_framework_client_server_runtime`, `test_cpp_framework_messaging` | Client/server and channel message paths use only binding public operations and preserve lifecycle results. |
| `test_cpp_framework_stream_framework` | The stream adapter maps connect, receive, and close ordering to framework session semantics. |

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../../../README.en.md) | [Previous: Runtime Architecture](../../common/internals/README.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:bottom:end -->
