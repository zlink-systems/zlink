package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.List;

public record ZLinkMeshNodeSnapshot(
    String meshName,
    ZLinkTopologyState state,
    boolean isReady,
    int readyPeerCount,
    List<ZLinkMeshChannelSnapshot> channels,
    List<ZLinkMeshPeerSnapshot> peers,
    ZLinkPlacementSnapshot placement,
    long sequence,
    Instant observedAt) {
    public ZLinkMeshNodeSnapshot {
        channels = List.copyOf(channels);
        peers = List.copyOf(peers);
    }
}
