package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.List;
import java.util.Objects;

public record ZLinkFanoutStatus(
    String channelName,
    ZLinkTopologyState state,
    boolean isReady,
    int readyPublisherCount,
    List<ZLinkMeshPeerSnapshot> publishers,
    long sequence,
    Instant observedAt) {
    public ZLinkFanoutStatus {
        Objects.requireNonNull(channelName, "channelName");
        Objects.requireNonNull(state, "state");
        publishers = List.copyOf(Objects.requireNonNull(publishers, "publishers"));
        Objects.requireNonNull(observedAt, "observedAt");
    }
}
