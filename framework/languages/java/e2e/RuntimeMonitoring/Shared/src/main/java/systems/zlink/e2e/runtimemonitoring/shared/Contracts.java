package systems.zlink.e2e.runtimemonitoring.shared;

import java.util.List;

public final class Contracts {
    public static final String CHANNEL = "monitoring.api";
    public static final String HANDSHAKE_CHANNEL = "monitoring.handshake";
    public static final String SPOT_MESH = "monitoring.spot.mesh";
    public static final String SPOT_CHANNEL = "monitoring.spot.runtime";
    public static final String MONITORING_SPOT_TYPE = "monitoring-room";
    public static final String TRIGGERED_MONITORING_SPOT_TYPE =
        "monitoring-triggered";
    public static final String HANDLER_GROUP = "monitoring";
    public static final String LOCATION_SOURCE = "ops-locations";

    private Contracts() {
    }

    public record WorkReq(String value) {
    }

    public record WorkRes(String value, String providerRid) {
    }

    public record SpotSubjectProbe(String value) {
    }

    public record EvidenceEntry(String surface, String sourceName, String event, String detail) {
    }

    public record EvidenceSnapshot(List<EvidenceEntry> entries) {
    }

    public record ObserverIsolationStatus(
        boolean started,
        long normalEventCount,
        long normalLatestSequence,
        long slowLatestSequence,
        boolean slowFailed) {
    }

    public record RuntimePeer(
        String nodeRid,
        String state,
        String unavailableReason) {
    }

    public record RuntimeChannel(
        String channelName,
        boolean ready,
        int readyTargetCount) {
    }

    public record RuntimeSnapshot(
        String meshName,
        String state,
        boolean ready,
        int readyPeerCount,
        long sequence,
        String observedAt,
        List<RuntimePeer> peers,
        List<RuntimeChannel> channels,
        boolean placementAvailable,
        int activeActorCount,
        int activeSpotCount,
        String placementUnavailableReason,
        String hostState) {
    }
}
