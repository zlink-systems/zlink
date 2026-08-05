package systems.zlink.e2e.spotactortransfer.shared;

import java.util.List;

public final class Contracts {
    public static final String MESH = "spot-actor-transfer";
    public static final String ENTRY_SPOT_RID = "spot-actor-transfer-entry";
    public static final String STATEFUL = "transfer-stateful";
    public static final String EMPTY_STATE = "transfer-empty-state";
    public static final String NO_ADAPTER = "transfer-no-adapter";
    public static final String FAIL_OUT = "transfer-fail-out";
    public static final String FAIL_LEAVE = "transfer-fail-leave";
    public static final String FAIL_IN = "transfer-fail-in";

    private Contracts() {
    }

    public record ActorCreateReq(String actorId, String actorType, int stateVersion) {
    }

    public record ActorCreateRes(String actorId, String actorType, String nodeRid, long generation) {
    }

    public record CreateSpotReq(String spotRid, String mode) {
    }

    public record CreateSpotRes(String spotRid, String nodeRid, String state) {
    }

    public record JoinTargetReq(String scenario, String targetSpotRid, String expectedMode) {
    }

    public record JoinTargetRes(
        String scenario,
        String actorId,
        boolean accepted,
        String sourceNodeRid,
        String targetSpotRid,
        int stateVersion) {
    }

    public record ProbeReq(String scenario, String marker) {
    }

    public record ProbeAtRefReq(
        String nodeRid,
        long generation,
        ProbeReq probe) {
    }

    public record ProbeWithTimeoutReq(
        String scenario,
        String marker,
        long timeoutMillis) {
    }

    public record MessageFollowSendReq(String scenario, String marker) {
    }

    public record SendAtRefReq(
        String nodeRid,
        long generation,
        MessageFollowSendReq message) {
    }

    public record ProbeRes(
        String scenario,
        String actorId,
        String spotRid,
        String nodeRid,
        int stateVersion,
        String marker) {
    }

    public record BindSessionReq(String scenario, String actorId) {
    }

    public record BindSessionRes(String scenario, String actorId, String nodeRid, long generation) {
    }

    public record BoundPushReq(String scenario, String marker) {
    }

    public record BoundPushRes(
        String scenario,
        String actorId,
        String spotRid,
        String nodeRid,
        String marker,
        int stateVersion) {
    }

    public record BoundPushNotify(
        String scenario,
        String actorId,
        String spotRid,
        String nodeRid,
        String marker,
        int stateVersion) {
    }

    public record GateReleaseRes(String key, boolean released) {
    }

    public record RetireRes(String intent, String outcome, String reason) {
    }

    public record TransferState(String actorId, int stateVersion, String actorType) {
    }

    public record Evidence(
        String scenario,
        String actorId,
        String kind,
        String value,
        String nodeRid,
        String transferId,
        String correlationId,
        String flowId) {
        public String text() {
            return String.join(
                "|", scenario, actorId, kind, value, nodeRid,
                transferId, correlationId, flowId);
        }
    }

    public record EvidenceSnapshot(List<Evidence> entries) {
    }

    public record ErrorResponse(String error) {
    }
}
