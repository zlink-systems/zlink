<!-- framework-adapter-nav:start -->
[Document List](../README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md) | [Regression Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:end -->

# Java Backend Dependency Policy

## 1. Purpose

The Java/Kotlin framework uses only `bindings/java`'s exported public raw socket API as its transport
boundary. The framework service runtime is shared by Java and Kotlin, and hides the binding
implementation objects and Core service objects.

## 2. Principles

- The framework public contract comes first.
- Binding concrete types are not directly exposed in the public API.
- Native handles, socket objects, monitor loops, and registry runtime objects are hidden inside the
  adapter.
- Only value types with language-neutral meaning, like `RoutingId`, `Message`, and `SendFlags`, can
  stay in the public contract.
- The framework calls only the Java binding's exported public raw socket API.
- `runtime.nativeapi`, package-private implementations, JNI symbols, and Core private symbols are not
  called.
- MeshNode, Spot, Actor, STREAM session, and the maintenance state machine are owned by the JVM
  service runtime.

## 3. Adapter Contract

Binding dependencies are isolated into a single group of internal ports in
`systems.zlink.framework.runtime.backend`. An adapter implementing this port on top of the public raw
binding is the JVM transport implementation. Ports and adapters are not part of the application public
interface and are not included in the Java module export.

| Port Interface (`systems.zlink.framework.runtime.backend`) | Role | Java Backend Implementation Target (`bindings/java`) |
|-----------------------------------------------|------|------------------------------------------|
| `ZLinkBackendAdapterFactory` | A factory that creates exactly 5 adapters | `ZLinkJavaBackendAdapterFactory` |
| `ZLinkChannelBackendAdapter` | Wraps the dealer/router/publisher/subscriber sockets, send/request, receive pump | dealer/router/publisher/subscriber socket |
| `ZLinkSpotBackendAdapter` | Wraps `SpotNode`/`Spot` (creates SpotNode) | spotNode/spot |
| `ZLinkStreamBackendAdapter` | Wraps the stream socket, session attach, frame send/reply | stream socket |
| `ZLinkMonitoringBackendAdapter` | Converts socket/discovery/registry/spot event sources into framework typed events | socket monitor/discovery events |

The factory has `createChannelAdapter`, `createSpotAdapter`, `createStreamAdapter`,

### 3.1 Java Binding Wrapper List

`.NET`'s `Runtime/Backend/DotNet/Wrappers/` has wrappers that hide binding objects inside the
framework. The Java adapter must implement the same wrappers 1:1 on top of `bindings/java`.

- context wrapper
- dealer socket wrapper
- router socket wrapper
- publisher socket wrapper
- subscriber socket wrapper
- discovery wrapper
- spotNode wrapper
- spot wrapper
- stream socket wrapper
- registry wrapper
- registryQueryClient wrapper
- socket monitor wrapper

These wrappers are created only inside the adapter. The binding concrete types they wrap do not leak
into the public surface.

The adapter lives in the framework's internal package. User guides and samples do not directly show
the adapter type.

The exact signature of the ports is owned by the JVM implementation internals. Other runtimes'
adapter structures are not copied into the JVM as a public abstraction.

## 4. What Must Not Leak Into The Public API

Except for the allowed primitives (`RoutingId`, `Message`, `SendFlags`), the following binding
concrete types and object models are not directly exposed in the framework public contract.

- binding `Context`
- dealer/router/publisher/subscriber socket, stream socket
- binding `Discovery`
- binding `SpotNode`, `Spot`
- native receive loop, monitor handle, timer handle
- internal frame encoder/decoder concrete types

Needed diagnostic values are re-interpreted as framework DTOs before exposure. Native enums or raw
status are kept only as optional details, and only when truly necessary.

## 5. Verification Criteria

| Test | Pass Criteria |
|--------|-----------|
| public surface backend leakage | Except for the allowed value types, no binding concrete type is in the public API |
| backend factory wrappers | The factory creates all 5 adapter kinds — channel, spot, stream, registry, monitoring — and wrapper creation stays inside the adapter |
| public raw binding only | No direct call to package-private, reflection, JNI, or Core private symbols |
| adapter-only transport construction | Raw sockets and monitors are created only inside the adapter |
| no Core service model | Core MeshNode, Spot, Actor, and session service types are absent from the JVM runtime dependency and public ABI |

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md) | [Regression Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:bottom:end -->
