<!-- framework-adapter-nav:start -->
[Document List](../README.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:end -->

[Java Docs](../README.en.md) | [Kotlin Docs](../../kotlin/README.en.md) | [Backend Policy](backend-dependency-policy.en.md)

# Java/Kotlin Framework Runtime Lifecycle

This document describes the Spring lifecycle and internal runtime ownership shared by Java and
Kotlin. The validation, timeout, cancellation, and reconnect contracts a user observes are owned by
each feature spec. The runtime structure shared by all four languages follows the
[Common Internal Structure](../../common/spec/server/README.en.md).

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

Sockets created by the Java Framework set `linger` to 30 seconds so a reply whose submission was
accepted can be transmitted before shutdown closes the socket. This value is not an Application
option; `ZLinkJavaSocketOptions` applies it while creating Framework-owned raw sockets. `linger`
limits the time available after socket close for the native transport to clean up outbound data that
it still holds.

This setting does not replace handler completion or confirmation that the remote runtime received the
reply. The runtime first closes new admission and completes already-accepted work with a terminal
result, then closes the socket. `linger` is therefore a transport cleanup condition that prevents an
accepted request reply from being discarded immediately during shutdown; it does not change
`reply.submit()` from submission acceptance into remote-delivery completion. If the shutdown deadline
expires, the common shutdown contract allows remaining accepted work to end with a shutdown result.

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
| `ChannelEgressRouting/CH-E2E-04B` | After ClientServer server shutdown, a request accepted before shutdown ends with a reply, and a new request is delivered to another ready server. |

## 5. Native Runtime Package Synchronization

The Java binding and Core package must use the same release version, `0.10.0`. When the Java binding
is verified or published, the Core package at
`/home/hep7/project/kairos/zlink/.artifacts/wsl/install/zlink-core/0.10.0` is supplied as the bridge
input, and the same provenance manifest and native runtime are used.

The Java binding checks the native runtime named by `ZLINK_LIBRARY_PATH` before using the test
classpath's source resource. If that variable is absent, a development native file left in the
source resource tree can be selected. After changing Core, rebuild the local package and synchronize
the source resource, or set `ZLINK_LIBRARY_PATH` to the verified runtime. Otherwise a Java test can
execute an older native binary, reproducing heap corruption or hiding the result of the fix.

The current poller lifetime fix does not hold `operation_sync` during a blocking wait. It blocks
registration mutation with `wait_active`, so a callback during readiness conversion can re-enter
the same poller API without deadlocking on the non-recursive mutex. Native registrations are
detached before poller destruction, and their ownership is released afterward. When a registered
socket is closed, its lifetime pin remains until the registration is removed and the poller returns
`POLLERR`. Java `NativePoller.close()` defers native destruction until the wait ends. The result is
checked with Core package verification, Java binding contract tests, and repeated runs with
`ZLINK_LIBRARY_PATH` explicitly set.

The direct cause of the heap corruption was that the Java `MONITOR_EVENT_LAYOUT` did not allocate
the diagnostic tail added to the Core monitor event while Core still wrote the complete structure.
The Java layout now includes `connection_id`, transport-pair identity, transport lane, and event
flags, reserving the full 816-byte Core structure. The current Java public `MonitorEvent` does not
expose those diagnostic values, but its receive buffer must still accommodate the complete native
public layout. `NativeLayoutsTest` fixes this size, and `MonitorBehaviorContractTest` verifies an
actual blocking receive.

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../README.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)
Location object queries preserve the last inspected position inside an authority page in an
opaque continuation token. Filtering or crossing the authority page boundary therefore does not
skip objects. The returned page is bounded by the encoded JSON size of 4 MiB; an individual entry
that cannot fit is reported as a failure instead of returning a partial success. `findSpotLocation`
accepts both user-Spot and instance-Spot authority rows.

<!-- framework-adapter-nav:bottom:end -->
