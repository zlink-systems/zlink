package systems.zlink.e2e.kotlin.automaticturn;

import java.util.List;

public final class Contracts {
    public static final String DELAY_CHANNEL = "await.dispatch.delay";
    public static final String SPOT_MESH = "await.dispatch.mesh";
    public static final String SPOT_RID_METADATA = "spot-rid";
    public static final String TARGET_NODE_RID_METADATA = "target-node-rid";

    private Contracts() {
    }

    public record DelayReq(String value, long millis) {
    }

    public record DelayRes(String value) {
    }

    public record ScenarioRes(String scenarioId, String requestId, String result) {
    }

    public record ProbeReq(String op, long millis) {
    }

    public record ProbeRes(String spotRid, String nodeRid, String op, String value) {
    }

    public record DispatchReq(String spotRid, String op, long millis) {
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

    public record EnsureSpotReq(String spotRid) {
    }

    public record EnsureSpotRes(String spotRid, String nodeRid) {
    }

    public record AwaitReq(String scenarioId, String requestId, String correlationId) {
    }

    public record ShutdownAwaitReq(String requestId, long delayMillis) {
    }

    public record AwaitShutdownRecoveryReq(String requestId, String spotRid) {
    }

    public record AwaitShutdownRecoveryRes(String operation, String spotRid, List<String> evidence) {
    }

    public record AwaitMsg(String requestId, long delayMillis, String correlationId) {
    }

    public record HoldMsg(String requestId, long delayMillis) {
    }

    public record WorkerAwaitMsg(String requestId, long delayMillis) {
    }

    public record ProbeMsg(String requestId, String marker) {
    }

    public record CleanupProbeReq(String requestId, String marker) {
    }

    public record CleanupProbeRes(String requestId) {
    }

    public record RemoteSpotAwaitReq(String requestId, String targetSpotRid, long delayMillis) {
    }

    public record AwaitTimeoutMsg(String requestId, long delayMillis, long timeoutMillis) {
    }

    public record AwaitTimeoutReq(String requestId, long delayMillis, long timeoutMillis) {
    }

    public record AwaitTimeoutRes(
        String scenarioId,
        String requestId,
        String spotRid,
        String nodeRid,
        boolean timedOut,
        String errorType) {
    }

    public record AwaitCancelMsg(String requestId, long delayMillis, long cancelAfterMillis) {
    }

    public record SpotProbeMsg(String requestId, String marker) {
    }

    public record EvidenceReq(String requestId) {
    }

    public record EvidenceRes(String requestId, List<String> markers) {
    }

    public record ActorAuthReq(String actorId) {
    }

    public record ActorAuthRes(String actorId) {
    }

    public record BindActorsReq(String spotRid, String actorA, String actorB) {
    }

    public record ActorBinding(String actorId, String nodeRid, long generation) {
    }

    public record BindActorsRes(
        String spotRid,
        String actorA,
        String actorB,
        List<ActorBinding> actors) {
    }

    public record ActorJoinReq(String spotRid, String value, long millis) {
        public ActorJoinReq(String spotRid, String value) {
            this(spotRid, value, 0);
        }
    }

    public record ActorJoinRes(String actorId, String spotRid, String value) {
    }

    public record ActorAwaitReq(String requestId, long delayMillis) {
    }

    public record ActorFastReq(String requestId, String marker) {
    }

    public record ActorJoinAwaitReq(String requestId, String targetSpotRid) {
    }

    public record ActorPushAwaitReq(String requestId, long delayMillis, String value) {
    }

    public record ActorPushNotify(String actorId, String requestId, String value, String nodeRid) {
    }

    public record ActorRes(String scenarioId, String requestId, String actorId, String marker) {
    }

}
