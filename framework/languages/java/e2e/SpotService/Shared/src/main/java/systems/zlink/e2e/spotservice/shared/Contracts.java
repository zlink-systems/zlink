package systems.zlink.e2e.spotservice.shared;

import java.util.List;

public final class Contracts {
    public static final String ROUTE_CHANNEL = "spot.service.route";
    public static final String ROUTE_PACKET = "RouteReq";
    public static final String EGRESS_CHANNEL = "spot.service.egress";
    public static final String INGRESS_CHANNEL = "spot.service.ingress";
    public static final String SPOT_MESH = "spot.service.mesh";
    public static final String SPOT_NODE = "play";

    private Contracts() {
    }

    public record StateReq(String op) {
    }

    public record StateRes(
        String spotRid,
        String nodeRid,
        String value) {
    }

    public record StateMsg(String value) {
    }

    public record SpotStateOperation(
        String spotRid,
        String value,
        long timeoutMilliseconds) {
    }

    public record SpotValueOperation(
        String spotRid,
        String value) {
    }

    public record RouteOperation(
        String nodeRid,
        String value,
        long timeoutMilliseconds) {
    }

    public record ActorOperation(
        String actorId,
        String value,
        long timeoutMilliseconds) {
    }

    public record OperationAccepted(boolean accepted) {
    }

    public record MissingSpotReq(String value) {
    }

    public record MissingSpotMsg(String value) {
    }

    public record CreateSpotReq(String spotRid) {
    }

    public record CreateSpotRes(
        String spotRid,
        String nodeRid,
        String state) {
    }

    public record PlacementWeightReq(int weight) {
    }

    public record PlacementWeightRes(int weight) {
    }

    public record RelocationReq() {
    }

    public record RelocationRes(String outcome, String reason) {
    }

    public record SpotStateRouteReq(
        String spotRid,
        String op,
        int delta) {
    }

    public record SpotMissingHandlerReq(String spotRid) {
    }

    public record SpotMissingHandlerRes(
        String spotRid,
        boolean failed,
        EvidenceSnapshot evidence) {
    }

    public record SpotMissingCommandReq(
        String spotRid,
        String marker) {
    }

    public record SpotMissingCommandRes(
        String spotRid,
        String marker,
        boolean sent,
        EvidenceSnapshot evidence) {
    }

    public record StageProbeReq(
        String marker,
        int delta) {
    }

    public record SpotStageProbeRouteReq(
        String spotRid,
        String marker,
        int delta) {
    }

    public record StageTimerStartReq(
        String name,
        int periodMilliseconds) {
    }

    public record SpotStageTimerRouteReq(
        String spotRid,
        String name,
        int periodMilliseconds) {
    }

    public record StageTimerStartRes(
        String spotRid,
        String name,
        boolean started) {
    }

    public record MultiNodeCreateSpotReq(
        String spotRid,
        int delta) {
    }

    public record MultiNodeCreateSpotRes(
        String spotRid,
        String nodeRid,
        String state,
        int value) {
    }

    public record MultiNodeStateRouteReq(
        String spotRid,
        int delta) {
    }

    public record MultiNodeStateReq(int delta) {
    }

    public record MultiNodeStateMsg(String marker) {
    }

    public record MultiNodeStateRes(
        String spotRid,
        String nodeRid,
        int value) {
    }

    public record SpotOnlyMeshReq(
        String sourceSpotRid,
        String targetSpotRid,
        String marker) {
    }

    public record SpotOnlyMeshRes(
        String sourceSpotRid,
        String targetSpotRid,
        int targetValue,
        String marker) {
    }

    public record SpotOnlyJoinReq(
        String targetSpotRid,
        String actorId,
        String marker) {
    }

    public record SpotOnlyJoinRes(
        String targetSpotRid,
        String actorId,
        boolean accepted,
        String marker) {
    }

    public record SlowReq(String value) {
    }

    public record OutboundReq(String value) {
    }

    public record OutboundRes(
        String spotRid,
        String nodeRid,
        String channelReply) {
    }

    public record OutboundMsg(String value) {
    }

    public record MeshMsg(String value) {
    }

    public record TimerActivityReq(String value) {
    }

    public record TimerActivityRes(
        String spotRid,
        String value) {
    }

    public record RouteReq(String value) {
    }

    public record RouteRes(
        String value,
        String nodeRid,
        String routeRid) {
    }

    public record ActorProfile(
        String displayName,
        int level,
        List<String> tags) {
    }

    public record ActorAuthReq(
        String actorId,
        ActorProfile profile) {
    }

    public record ActorAuthRes(
        String actorId,
        String nodeRid,
        long generation,
        int boundCount,
        String displayName,
        int level,
        List<String> tags) {
    }

    public record MultiBindReq(
        String firstActorId,
        String secondActorId,
        ActorProfile profile) {
    }

    public record MultiBindRes(int boundCount) {
    }

    public record ActorJoinReq(
        String spotRid,
        ActorProfile profile,
        List<String> tags) {
    }

    public record JoinAdmittedUserSpotActorReq(
        String spotRid,
        ActorProfile profile,
        List<String> tags,
        boolean admit,
        String reason) {
    }

    public record ActorJoinRes(
        String actorId,
        String spotRid,
        String nodeRid,
        String displayName,
        int level,
        List<String> tags) {
    }

    public record JoinAdmittedUserSpotActorRes(
        String actorId,
        String spotRid,
        String nodeRid,
        boolean accepted,
        String errorKind) {
    }

    public record LeaveActorReq(String actorId) {
    }

    public record LeaveActorRes(
        String actorId,
        boolean accepted) {
    }

    public record ActorDestroyReq(String actorId) {
    }

    public record ActorDestroyRes(
        String actorId,
        boolean destroyed) {
    }

    public record ActorEchoReq(
        String value,
        int seq,
        ActorProfile profile) {
    }

    public record MissingActorReq(String value) {
    }

    public record ActorPingReq(String value) {
    }

    public record ActorPingRes(
        String actorId,
        String nodeRid,
        String spotRid,
        String value,
        int seen) {
    }

    public record ActorPushReq(String value) {
    }

    public record SnapshotReq(String actorId) {
    }

    public record SnapshotRes(
        String actorId,
        int seen) {
    }

    public record SlowSessionReq(
        String value,
        int delayMilliseconds) {
    }

    public record SlowSessionRes(String value) {
    }

    public record ActorEchoRes(
        String actorId,
        String spotRid,
        String nodeRid,
        String value,
        int requestSeq,
        int handlerSeq,
        String displayName,
        int level,
        List<String> tags) {
    }

    public record ActorPushNotify(
        String actorId,
        String spotRid,
        String value,
        int requestSeq,
        int handlerSeq) {
    }

    public record EvidenceEntry(
        String marker,
        String nodeRid,
        String spotRid,
        String value) {
    }

    public record EvidenceWaitReq(
        List<String> containsAll,
        int timeoutMilliseconds) {
    }

    public record EvidenceSnapshot(
        String nodeRid,
        List<EvidenceEntry> entries) {
    }

}
