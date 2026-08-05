package systems.zlink.e2e.automaticturn.shared;

import java.util.List;

public final class Contracts {
    public static final String DELAY_CHANNEL = "automatic.turn.delay";
    public static final String ROUTE_CHANNEL = "automatic.turn.route";
    public static final String SPOT_MESH = "automatic.turn.spot";
    public static final String TARGET_SPOT = "await-probe";
    public static final String PLAY_NODE_A = "play-a";
    public static final String PLAY_NODE_B = "play-b";
    public static final String PLAY_NODE = PLAY_NODE_A;
    public static final String SPOT_RID_METADATA = "spot-rid";
    public static final String TARGET_NODE_RID_METADATA = "target-node-rid";
    public static final String ACTOR_ID_METADATA = "actor-id";
    public static final String ACTOR_TYPE = "await.actor";
    public static final String OBS_FANOUT_CHANNEL = "observability.track.a";

    private Contracts() {
    }

    public record ScenarioReq(String scenarioId, String requestId) {
    }

    public record ScenarioRes(String scenarioId, String requestId, String result) {
    }

    public record DelayReq(String requestId, long delayMillis) {
    }

    public record DelayRes(String requestId) {
    }

    public record HoldReq(String requestId) {
    }
    public record ObservabilityQueueReq(String requestId) {
    }

    public record AwaitReq(String scenarioId, String requestId, String correlationId) {
    }

    public record AwaitMsg(String requestId, long delayMillis, String correlationId) {
    }

    public record AwaitTimeoutMsg(String requestId, long delayMillis, long timeoutMillis) {
    }

    public record AwaitCancelMsg(String requestId, long delayMillis, long cancelAfterMillis) {
    }

    public record AwaitShutdownScenarioReq(String requestId, String spotRid, long delayMillis) {
    }

    public record AwaitShutdownRecoveryReq(String requestId, String spotRid) {
    }

    public record AwaitShutdownRes(String operation, String requestId, String spotRid) {
    }

    public record WorkerAwaitReq(String requestId) {
    }

    public record WorkerAwaitMsg(String requestId, long delayMillis) {
    }

    public record ProbeReq(String requestId) {
    }

    public record ProbeMsg(String requestId, String marker) {
    }

    public record TimerStartMsg(
        String requestId,
        String timerName,
        String mode,
        long periodMillis,
        long delayMillis) {
    }

    public record TimerStopMsg(String requestId) {
    }

    public record ObservabilityFanoutEvent(String requestId, long deliveryIndex) {
    }

    public record EnsureSpotReq(String spotRid) {
    }

    public record EnsureSpotRes(String spotRid, String nodeRid) {
    }

    public record PersistentRoomStateReq(String value, boolean append) {
    }

    public record PersistentRoomStateRes(
        String spotRid,
        String value,
        int eventCount,
        boolean replayed,
        String nodeRid) {
    }

    public record RemoteSpotAwaitReq(String requestId, String targetSpotRid, long delayMillis) {
    }

    public record ProbeRes(String requestId) {
    }

    public record BindActorsReq(String spotRid, String actorA, String actorB) {
    }

    public record ActorBinding(String actorId, String nodeRid, long generation) {
    }

    public record BindActorsRes(String spotRid, String actorA, String actorB, List<ActorBinding> actors) {
    }

    public record ActorAwaitReq(String requestId, long delayMillis) {
    }

    public record ActorFastReq(String requestId, String marker) {
    }

    public record ActorJoinReq(String requestId, String spotRid) {
    }

    public record ActorJoinAwaitReq(String requestId, String targetSpotRid) {
    }

    public record ActorPushAwaitReq(String requestId, long delayMillis, String value) {
    }

    public record ActorPushNotify(String actorId, String requestId, String value, String nodeRid) {
    }

    public record ActorAwaitRes(String scenarioId, String requestId, String actorId, String marker) {
    }

    public record ActorFastRes(String scenarioId, String requestId, String actorId, String marker) {
    }

    public record ActorJoinRes(String scenarioId, String requestId, String actorId, String marker) {
    }

    public record ActorJoinAwaitRes(String scenarioId, String requestId, String actorId, String marker) {
    }

    public record ActorPushAwaitRes(String scenarioId, String requestId, String actorId, String marker) {
    }

    public record EvidenceEntry(long sequence, String marker, String subject, String value) {
    }

    public record EvidenceSnapshot(String nodeRid, List<EvidenceEntry> entries) {
    }
}
