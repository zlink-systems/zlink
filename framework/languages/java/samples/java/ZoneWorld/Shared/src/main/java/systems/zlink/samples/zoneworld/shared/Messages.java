package systems.zlink.samples.zoneworld.shared;

import java.util.List;

public final class Messages {
    private Messages() {
    }

    public record PlayerView(String playerId, int x, int y, String zoneId, boolean isBot) {
    }

    public record NodeView(
        String nodeId,
        boolean registered,
        boolean connected,
        boolean maintenance,
        List<String> zones,
        int playerCount) {
    }

    public record JoinWorldMsg(String playerId) {
    }

    public record JoinWorldNotify(String playerId, String zoneId, int x, int y, String error) {
    }

    public record MoveMsg(int x, int y) {
    }

    public record ZoneStateNotify(String zoneId, long tick, List<PlayerView> players) {
    }

    public record ZoneChangedNotify(String playerId, String zoneId) {
    }

    public record WorldAnnounceNotify(String announcementId, String text) {
    }

    public record MoveRejectedNotify(String reason, int x, int y) {
    }

    public record WatchNodesReq() {
    }

    public record WatchNodesRes(List<NodeView> nodes) {
    }

    public record NodeStatusNotify(
        String nodeId,
        boolean registered,
        boolean connected,
        boolean maintenance,
        List<String> zones,
        int playerCount) {
    }

    public record NodeAlertNotify(String nodeId, String kind, String detail, String occurredAt) {
    }

    public record AnnounceWorldReq(String text) {
    }

    public record AnnounceWorldRes(String announcementId) {
    }

    public record SetMaintenanceReq(String nodeId, boolean enabled) {
    }

    public record SetMaintenanceRes(String nodeId, boolean enabled, List<String> zones, String error) {
    }

    public record NodeDiagnosticsReq(String nodeId) {
    }

    public record NodeDiagnosticsRes(
        String nodeId,
        List<String> zones,
        int playerCount,
        boolean maintenance,
        String error) {
    }

    public record RelocationPairReq() {
    }

    public record RelocationPairRes(
        String sourceZoneId,
        String targetZoneId,
        String sourceOwnerNodeRid,
        String targetOwnerNodeRid,
        String error) {
    }

    public record ActorLocationProbeReq(String actorId) {
    }

    public record ActorLocationProbeRes(
        String actorId,
        long objectGeneration,
        String ownerNodeRid,
        String error) {
    }

    public record FreshActorProbeReq(String actorId) {
    }

    public record FreshActorProbeRes(
        String actorId,
        long objectGeneration,
        String ownerNodeRid,
        String error) {
    }

    public record WorldAnnounceEvent(String announcementId, String text) {
    }

    public record NodeMaintenanceChangedEvent(String nodeId, boolean enabled) {
    }

    public record DeliverAnnounceMsg(String announcementId, String text) {
    }

    public record BotTickMsg() {
    }

    public record EnterWorldReq(int x, int y, boolean isBot, int dirX, int dirY) {
    }

    public record EnterWorldRes(String zoneId, int x, int y, String error) {
    }

    public record ReportSpotEventMsg(String nodeId, String kind, String detail, String occurredAt) {
    }

    public record ReportNodeStatusMsg(
        String nodeId,
        List<String> zones,
        int playerCount,
        boolean maintenance) {
    }

    public record ZoneBorderEvent(
        String fromZoneId,
        String toZoneId,
        long tick,
        List<PlayerView> players) {
    }

    public record EnterZoneReq(
        String playerId,
        int x,
        int y,
        boolean isBot,
        boolean initialEntry,
        String fromZoneId,
        boolean crashBoundaryProbe) {
    }

    public record EnterZoneRes(String zoneId, String error) {
    }

    public record UpdatePositionMsg(String playerId, int x, int y, boolean isBot) {
    }

    public record DeliverZoneStateMsg(String zoneId, long tick, List<PlayerView> players) {
    }

    public record DeliverZoneChangedMsg(String playerId, String zoneId) {
    }

    public record DeliverWorldAnnounceMsg(String announcementId, String text) {
    }

    public record MessageFollowProbeReq(String actorId, String probeId, byte[] payload) {
    }

    public record MessageFollowProbeRes(String probeId, byte[] payload) {
    }

    public record MessageFollowProbeMsg(String actorId, String probeId, byte[] payload) {
    }

    public record CrashRelocationProbeMsg(int x, int y) {
    }

    public record CrashRelocationProbeRes(String error) {
    }
}
