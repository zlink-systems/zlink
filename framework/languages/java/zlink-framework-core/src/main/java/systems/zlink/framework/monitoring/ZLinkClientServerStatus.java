package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.List;
import java.util.Objects;

public record ZLinkClientServerStatus(
    String channelName,
    ZLinkClientServerRole localRole,
    ZLinkTopologyState state,
    boolean isReady,
    int readyTargetCount,
    List<ZLinkClientServerTargetStatus> targets,
    long sequence,
    Instant observedAt) {
    public ZLinkClientServerStatus {
        Objects.requireNonNull(channelName, "channelName");
        Objects.requireNonNull(localRole, "localRole");
        Objects.requireNonNull(state, "state");
        targets = List.copyOf(Objects.requireNonNull(targets, "targets"));
        Objects.requireNonNull(observedAt, "observedAt");
    }
}
