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

## 4. Regression Tests

| Test Case | Pass Criteria |
|---------------|-----------|
| `test_cpp_framework_layout_contract` | Public headers expose no private runtime or backend concrete dependency. |
| `test_cpp_framework_contract_headers` | Public headers compile with no private include path. |
| `test_cpp_framework_native_backend` | The backend adapter connects channel, Spot, and stream through public binding operations. |

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../../../README.en.md) | [Previous: Runtime Architecture](../../common/internals/README.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:bottom:end -->
