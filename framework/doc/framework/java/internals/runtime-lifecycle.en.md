<!-- framework-adapter-nav:start -->
[Document List](../README.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:end -->

[Java Docs](../README.en.md) | [Kotlin Docs](../../kotlin/README.en.md) | [Backend Policy](backend-dependency-policy.en.md)

# Java/Kotlin Framework Runtime Lifecycle

This document describes the Spring lifecycle and internal runtime ownership shared by Java and
Kotlin. The validation, timeout, cancellation, and reconnect contracts a user observes are owned by
each feature spec. The runtime structure shared by all four languages follows the
[Common Internal Structure](../../common/internals/README.en.md).

## 1. Start Order

1. Spring auto-configuration builds registrations from options and handler scanner results.
2. The registration validator checks channel, Spot, stream, and handler combinations.
3. The starter's non-public bean assembly lives in the same implementation package as
   `ZLinkFrameworkRuntime`. This assembly calls the package-private bootstrap to create a single
   facade bean in the `PREPARING` state. At this stage, sockets, discovery loops, and application
   workers are not started.
4. The runtime creates and owns one RouteMesh, ClientServer, and fanout monitoring view each.
   `routeMeshRuntime()`, `clientServerRuntime()`, and `fanoutRuntime()` return the same object for the
   runtime's lifetime.
5. The bean assembly takes the public client and the three topology views from the runtime and
   registers them as singleton beans. Topology beans use the objects the accessor returns as-is,
   without building a separate adapter.
6. A non-public `SmartLifecycle` adapter in the same implementation package keeps a runtime
   reference. The adapter calls the package-private start boundary and does not create or replace the
   facade.
7. `SmartLifecycle.start()` calls the runtime's start boundary exactly once.
8. The runtime builds the backend adapter using the Java binding's public raw socket API and the JVM
   service context, then starts location, channel, route, Spot, stream, and monitoring in order.

Creating the Spring bean alone does not partially start the service runtime. The actual start is
owned by the framework's `SmartLifecycle`, and if it fails mid-start, already-created resources are
cleaned up in reverse order. The Application is injected only the stabilized facade bean. The runtime
reports `PREPARING` until start finishes, and application operations do not implicitly start the
runtime.

Bootstrap and the start boundary are package-private, and provide no public constructor, public
factory, or public accessor that pulls the runtime out of the lifecycle. The framework implementation
does not bypass this boundary with reflection, `MethodHandles`, or private-member access. The concrete
types of the auto-configuration class, bean factory method, and lifecycle adapter are not part of the
application public contract.

## 2. Shutdown Order

Spring `SmartLifecycle` shutdown calls `ZLinkFrameworkRuntime.shutdown()`. Operational maintenance
calls `retire()`. The runtime blocks new dispatch entry and cleans up accepted work, STREAM barrier,
monitoring, Spot, route, stream, channel, location, and backend context in order. Pending completions
and coroutine continuations are each completed or failed by their runtime owner. It does not occupy
JVM threads or coroutine dispatchers with a blocking wait.

`retire()` and `shutdown()` target the entire host. The deprecated `drain()` and `awaitDrained()` are
compatibility facades that use the same host `shutdown()` operation. There is no partial-termination
operation that takes a MeshName — a single topology is not terminated separately.

## 3. The Java/Kotlin Shared Boundary

Kotlin handlers share the Java runtime's registration and execution queue. Only the wrapper that
connects `suspend` continuations to the coroutine context is Kotlin-specific — it does not build a
separate service runtime or lifecycle. The shared runtime's RouteMesh/ClientServer probe and Fanout
liveness criteria follow the common runtime monitoring spec and the current binding package
configuration.

## 4. Regression Tests

| Test Case | Pass Criteria |
|---------------|-----------|
| `HostTest.host_startsAndStops_frameworkRuntimeContext` | The Spring lifecycle and framework context creation/cleanup work together. |
| `ZLinkFrameworkAutoConfigurationTest.autoConfigurationStartsFrameworkLifecycleAndExposesClientBean` | Auto-configuration connects the lifecycle to the public client bean. |
| `ZLinkFrameworkAutoConfigurationTest.exposesSingleRuntimeAndTopologyRuntimeBeans` | `ZLinkFrameworkRuntime` and the three topology view beans are provided as singletons, and each bean matches the facade accessor's return value via `assertSame`. |
| `HostTest.smartLifecycleStartsAndStopsTheSameFrameworkRuntimeBean` | The `SmartLifecycle` adapter starts/shuts down the injected runtime bean without replacing it. |
| `HostTest.beanCreationDoesNotStartRuntime` | Bean creation configures the runtime as `PREPARING` but does not start sockets, discovery loops, or workers. |
| `ZLinkAsyncSubmitterTest.close_failsPendingItems` | Runtime shutdown does not leave pending submits behind. |
| `KotlinSuspendAnnotationHandlerTest.kotlinSuspendAnnotationCancellationCompletesJavaStageExceptionally` | Kotlin cancellation propagates to the shared Java completion. |

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../README.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:bottom:end -->
