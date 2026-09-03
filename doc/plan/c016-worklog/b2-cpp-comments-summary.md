# C++ projection receive-flow comment update

Scope: comments and docstrings only. No code, signature, enum value, `doc/**`, `core/**`, or `framework/**` content was changed. No build or test was run.

## Updated files and lines

- `bindings/cpp/include/zlink/Contracts/Eventing/events.hpp`:37
- `bindings/cpp/include/zlink/Contracts/Eventing/status.hpp`:37, 133
- `bindings/cpp/include/zlink/Contracts/Sockets/socket_options.hpp`:38, 44
- `bindings/cpp/include/zlink/Contracts/Sockets/socket_contracts.hpp`:73
- `bindings/dotnet/src/Zlink/Contracts/Core/CoreHwmBudgetSnapshot.cs`:50, 52
- `bindings/dotnet/src/Zlink/Contracts/Eventing/EventEnums.cs`:93, 109, 209, 225
- `bindings/dotnet/src/Zlink/Contracts/Eventing/Monitor.cs`:224
- `bindings/dotnet/src/Zlink/Contracts/Sockets/ISocket.cs`:74
- `bindings/dotnet/src/Zlink/Contracts/Sockets/SocketEnums.cs`:84
- `bindings/go/contracts/eventing.go`:75, 77, 113, 115, 131
- `bindings/go/contracts/sockets.go`:14, 87, 89
- `bindings/go/internal/native/monitor.go`:35, 38, 216
- `bindings/go/internal/native/socket_types.go`:48
- `bindings/go/monitor_test.go`:85, 100, 104
- `bindings/java/src/main/java/systems/zlink/contracts/eventing/EventEnums/MonitorEventType.java`:43, 49
- `bindings/java/src/main/java/systems/zlink/contracts/sockets/SocketEnums/ReceiveFlowState.java`:6
- `bindings/node/src/zlink/contracts/eventing/monitor.ts`:27, 29, 104
- `bindings/node/src/zlink/contracts/sockets/socket_constants.ts`:28
- `bindings/python/src/zlink/_native/ffi.py`:140
- `bindings/python/src/zlink/_runtime/sockets/socket_base.py`:425, 431
- `bindings/python/src/zlink/contracts/eventing/monitor.py`:19
- `bindings/python/src/zlink/contracts/sockets/codes.py`:91
- `bindings/python/tests/test_core_api_alignment.py`:126
- `bindings/python/tests/test_flow_state_parity.py`:250
- `bindings/rust/src/contracts/eventing/monitor.rs`:34, 227
- `bindings/rust/src/contracts/sockets/socket_options.rs`:71, 169
- `bindings/rust/src/runtime/native/ffi.rs`:46, 482
- `bindings/rust/src/runtime/sockets/socket/socket_inner_runtime.rs`:452
- `bindings/rust/tests/monitor_tests.rs`:219

The revised descriptions state that receive-flow is supported by DEALER/ROUTER socket types; control uses the Application connection for count-1 peers and the Completion connection for count-2 ROUTER-ROUTER peers; monitor events and telemetry describe affected Application pipes.

## Searched but not changed

- `bindings/cpp/tests/contract/test_cpp_contract_flow_state.cpp`:79,328: C++ test comments are outside the requested four C++ projection headers and the cross-language source-comment scope.
- `bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs`:332: performance-test comment refers to Core request-terminal queuing, not receive-flow control.
- `bindings/python/src/zlink/contracts/eventing/monitor.py`:116: `application/completion lane` is the existing `transport_lane` API vocabulary, not the removed paired-DEALER/ROUTER receive-flow claim.

## Verification

`git diff --check`

```text
exit 0; no output
```

`git diff --stat`

```text
 bindings/cpp/include/zlink/Contracts/Eventing/events.hpp |  6 +++---
 bindings/cpp/include/zlink/Contracts/Eventing/status.hpp | 10 +++++-----
 .../include/zlink/Contracts/Sockets/socket_contracts.hpp | 13 +++++++------
 .../include/zlink/Contracts/Sockets/socket_options.hpp   |  8 +++++---
 .../src/Zlink/Contracts/Core/CoreHwmBudgetSnapshot.cs    |  4 ++--
 .../dotnet/src/Zlink/Contracts/Eventing/EventEnums.cs    | 16 ++++++++--------
 bindings/dotnet/src/Zlink/Contracts/Eventing/Monitor.cs  |  2 +-
 bindings/dotnet/src/Zlink/Contracts/Sockets/ISocket.cs   |  5 +++--
 .../dotnet/src/Zlink/Contracts/Sockets/SocketEnums.cs    |  5 +++--
 bindings/go/contracts/eventing.go                        | 10 +++++-----
 bindings/go/contracts/sockets.go                         |  6 +++---
 bindings/go/internal/native/monitor.go                   | 13 +++++++------
 bindings/go/internal/native/socket_types.go              |  5 +++--
 bindings/go/monitor_test.go                              |  9 +++++----
 .../contracts/eventing/EventEnums/MonitorEventType.java  |  7 +++----
 .../contracts/sockets/SocketEnums/ReceiveFlowState.java  |  5 +++--
 bindings/node/src/zlink/contracts/eventing/monitor.ts    |  8 ++++----
 .../node/src/zlink/contracts/sockets/socket_constants.ts |  6 ++++--
 bindings/python/src/zlink/_native/ffi.py                 |  5 +++--
 .../python/src/zlink/_runtime/sockets/socket_base.py     |  9 +++++----
 bindings/python/src/zlink/contracts/eventing/monitor.py  |  4 ++--
 bindings/python/src/zlink/contracts/sockets/codes.py     |  5 +++--
 bindings/python/tests/test_core_api_alignment.py         |  2 +-
 bindings/python/tests/test_flow_state_parity.py          |  2 +-
 bindings/rust/src/contracts/eventing/monitor.rs          |  6 +++---
 bindings/rust/src/contracts/sockets/socket_options.rs    |  9 ++++++---
 bindings/rust/src/runtime/native/ffi.rs                  |  9 ++++++---
 .../src/runtime/sockets/socket/socket_inner_runtime.rs   |  5 +++--
 bindings/rust/tests/monitor_tests.rs                     |  2 +-
 29 files changed, 108 insertions(+), 88 deletions(-)
```

No build or test was run, as requested.
