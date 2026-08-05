# Java Monitoring Public Interface

[Interface table of contents](README.en.md) ·
[Runtime Monitoring](../../../../24-runtime-monitoring.en.md)

## 1. Scope

The application queries the current state of host and topology, and
observes the order in which the complete state changes. The lifecycle
generation, descriptor source, endpoint, admission/claim/reservation,
and pending work the framework uses to judge ownership and connections
aren't exposed.

## 2. Host State

```java
package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.Optional;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult;

public record ZLinkObservationLoss(
    long coalescedCount,
    long discardedTerminalCount) {}

public record ZLinkObservedStatus<T>(
    T status,
    ZLinkObservationLoss loss) {}

public record ZLinkInboundDispatchStatus(
    long applicationHwmBytes,
    long pendingPayloadBytes,
    long queuedPayloadBytes,
    long activePayloadBytes,
    boolean applicationReceivePaused,
    long pendingCompletionSends,
    long completionSendLimit) {}

public record ZLinkFrameworkRuntimeStatus(
    ZLinkFrameworkRuntimeState state,
    boolean isReady,
    boolean acceptingWork,
    Optional<Instant> deadline,
    Optional<ZLinkFrameworkRelocationResult> relocationResult,
    Optional<ZLinkFrameworkTerminationResult> terminationResult,
    ZLinkInboundDispatchStatus inboundDispatch,
    long sequence,
    Instant observedAt) {}
```

The interface that starts relocation and shutdown is determined by the
host lifecycle contract. Monitoring doesn't provide separate drain
control or a per-component termination result.

## 3. RouteMesh State

```java
package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.Flow;
import systems.zlink.contracts.core.RoutingId;

public enum ZLinkTopologyState {
    STARTING,
    READY,
    DEGRADED,
    STOPPING,
    STOPPED,
    FAILED
}

public enum ZLinkTopologyReason {
    RUNTIME_NOT_READY,
    NO_READY_PEER,
    NO_READY_TARGET,
    LOCATION_UNAVAILABLE,
    CAPACITY_EXCEEDED,
    DRAINING,
    INTERNAL_FAILURE
}

public enum ZLinkPeerState {
    CONNECTING,
    READY,
    DRAINING,
    NOT_CONNECTED,
    NOT_REQUIRED
}

public record ZLinkMeshPeerSnapshot(
    RoutingId nodeRid,
    ZLinkPeerState state,
    Optional<ZLinkTopologyReason> unavailableReason) {}

public record ZLinkMeshChannelSnapshot(
    String channelName,
    boolean isReady,
    int readyTargetCount) {}

public record ZLinkPlacementSnapshot(
    boolean isAvailable,
    int activeActorCount,
    int activeSpotCount,
    Optional<ZLinkTopologyReason> unavailableReason) {}

public record ZLinkMeshNodeSnapshot(
    String meshName,
    ZLinkTopologyState state,
    boolean isReady,
    int readyPeerCount,
    List<ZLinkMeshChannelSnapshot> channels,
    List<ZLinkMeshPeerSnapshot> peers,
    ZLinkPlacementSnapshot placement,
    long sequence,
    Instant observedAt) {}

public interface ZLinkRouteMeshRuntime {
    ZLinkMeshNodeSnapshot snapshot(String meshName);

    Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> observe(
        String meshName,
        int capacity);

    boolean isReady(String meshName);
}
```

The unit `observe(...)` delivers is `ZLinkObservedStatus<T>`. `status`
delivers a complete snapshot after a change, not an event holding only
some fields, and is shared across subscribers. `loss` is a loss
accumulator specific to this one subscription, so it isn't put inside
status. Intermediate states can be coalesced due to a slow subscriber, in
which case the latest state of a kept source isn't omitted. A terminal
state isn't overwritten by an intermediate state, but if the retention
cap is exceeded, the oldest terminal is discarded first and the count is
reported to the observer
([Runtime Status Query And Operational Diagnostics](../../../../24-runtime-monitoring.en.md)).

`ZLinkObservationLoss.coalescedCount` is the number of intermediate
states this subscriber didn't see because of per-source latest-slot
coalescing, and `discardedTerminalCount` is the number of terminal
states discarded for exceeding the retention cap. The two aren't merged
into one — because a subscriber must distinguish "skipped by catching
up" from "never seen at all." Both values start at `0` per `observe(...)`
subscription, increase monotonically within the same subscription, and
are pinned at `Long.MAX_VALUE` (`2^63 - 1`) once exceeded. This cap is
the same across all four languages. The framework doesn't complete or
error-terminate the `Flow.Publisher` just because the subscriber's queue
is full. The definition of the delivery unit is owned by
[Runtime Monitoring §3](../../../../24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

Placement's `isAvailable` is only `true` when the host is `SERVING` and
Object Server, node-wide placement weight is positive, and both Actor
and Spot capacity and activation concurrency have room. The current
value and limit of activation aren't exposed in public status.

Peer state only provides `nodeRid`, `state`, `unavailableReason`.
`NOT_REQUIRED` is a normal state where two Object Clients don't need to
connect because there's no RouteMesh Channel Server membership. This
state is excluded from ready peer count and liveness failure
aggregation.

## 4. ClientServer State

```java
package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.Flow;
import systems.zlink.contracts.core.RoutingId;

public enum ZLinkClientServerRole {
    CLIENT,
    SERVER,
    CLIENT_AND_SERVER
}

public record ZLinkClientServerTargetStatus(
    RoutingId nodeRid,
    int weight,
    ZLinkPeerState state,
    Optional<ZLinkTopologyReason> unavailableReason) {}

public record ZLinkClientServerStatus(
    String channelName,
    ZLinkClientServerRole localRole,
    ZLinkTopologyState state,
    boolean isReady,
    int readyTargetCount,
    List<ZLinkClientServerTargetStatus> targets,
    long sequence,
    Instant observedAt) {}

public interface ZLinkClientServerRuntime {
    ZLinkClientServerStatus snapshot(String channelName);

    Flow.Publisher<ZLinkObservedStatus<ZLinkClientServerStatus>> observe(
        String channelName,
        int capacity);

    boolean isReady(String channelName);
}
```

A Server on the same process also applies the same weight rule as a
remote Server. Target status isn't an API that forces target selection
or changes weight.

## 5. Automatic Fanout State

```java
package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.List;
import java.util.concurrent.Flow;

public record ZLinkFanoutStatus(
    String channelName,
    ZLinkTopologyState state,
    boolean isReady,
    int readyPublisherCount,
    List<ZLinkMeshPeerSnapshot> publishers,
    long sequence,
    Instant observedAt) {}

public interface ZLinkFanoutRuntime {
    ZLinkFanoutStatus snapshot(String channelName);

    Flow.Publisher<ZLinkObservedStatus<ZLinkFanoutStatus>> observe(
        String channelName,
        int capacity);
}
```

Connection intent, discovery source, and lifecycle generation are
framework-internal state. The application only confirms the publisher's
Node RID and current peer state.

## 6. Runtime Error Recording

An error occurring in an internal runtime callback or observer is
recorded by the framework as a structured log. An error sink and raw
event DTO that the application implements or registers aren't the
public contract.
