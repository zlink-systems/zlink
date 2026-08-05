# Java monitoring 공개 인터페이스

[인터페이스 목차](README.ko.md) ·
[Runtime monitoring](../../../../24-runtime-monitoring.ko.md)

## 1. 범위

Application은 host와 topology의 현재 상태를 조회하고, 완전한 상태가 바뀌는 순서를 관찰한다.
Framework가 ownership과 연결을 판정하는 lifecycle generation, descriptor source, endpoint,
admission·claim·reservation과 pending work는 공개하지 않는다.

## 2. Host 상태

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

Relocation과 shutdown을 시작하는 interface는 host lifecycle 계약이 정한다. Monitoring은 별도 drain
control이나 component별 termination result를 제공하지 않는다.

## 3. RouteMesh 상태

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

`observe(...)`가 전달하는 단위는 `ZLinkObservedStatus<T>`다. `status`는 일부 field만 담은 event가 아니라 변경 뒤의 완전한 snapshot을 전달한다. subscriber 사이에 공유한다. `loss`는 이 subscription 하나에만 해당하는
유실 누계이므로 status 안에 넣지 않는다. 느린
subscriber 때문에 중간 상태를 합칠 수 있으며, 이때 보관 중인 source의 최신 상태는 생략하지
않는다. Terminal 상태는 중간 상태로 덮어쓰지 않지만, 보관 상한을 넘기면 오래된 terminal부터
버리고 그 수를 관찰자에게 알린다
([Runtime 상태와 운영 진단](../../../../24-runtime-monitoring.ko.md)).

`ZLinkObservationLoss.coalescedCount`는 source별 최신 slot 합치기로 이 subscriber가 보지 못한 중간 상태
수이고, `discardedTerminalCount`는 보관 상한 초과로 폐기한 terminal 상태 수다. 둘을 하나로 합치지
않는다 — subscriber가 "따라잡기로 건너뛴 것"과 "영영 못 보는 것"을 구분해야 하기 때문이다. 두 값은
`observe(...)` 구독마다 `0`에서 시작하고 같은 구독 안에서 단조 증가하며, `Long.MAX_VALUE`(`2^63 - 1`)를 넘으면 그 값으로 고정한다. 이 상한은 네 언어가 같다. Framework는 subscriber queue가 가득 찼다는 이유로 `Flow.Publisher`를
완료하거나 오류로 끝내지 않는다. 전달 단위의 정의는
[Runtime monitoring §3](../../../../24-runtime-monitoring.ko.md#3-현재-상태-조회와-변화-관찰)이 소유한다.

Placement의 `isAvailable`은 host가 `SERVING`이고 Object Server이며, node-wide placement
weight가 양수이고, Actor 또는 Spot capacity와 activation concurrency에 모두 여유가 있을 때만
`true`다. Activation의 현재 값과 limit은 public status에 노출하지 않는다.

Peer 상태는 `nodeRid`, `state`, `unavailableReason`만 제공한다. `NOT_REQUIRED`는 두 Object Client
사이에 RouteMesh Channel Server membership이 없어 연결할 필요가 없는 정상 상태다. 이 상태는
ready peer 수와 liveness failure 집계에서 제외한다.

## 4. ClientServer 상태

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

같은 process의 Server도 remote Server와 같은 weight 규칙을 적용한다. Target status는 target 선택을
강제하거나 weight를 변경하는 API가 아니다.

## 5. Automatic fanout 상태

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

Connection intent, discovery source와 lifecycle generation은 Framework 내부 상태다. Application은
publisher의 Node RID와 현재 peer 상태만 확인한다.

## 6. Runtime 오류 기록

Runtime 내부 callback이나 observer에서 발생한 오류는 Framework가 structured log로 기록한다.
Application이 구현하거나 등록하는 error sink와 raw event DTO는 public contract가 아니다.
