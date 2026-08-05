package systems.zlink.framework.runtime.internal.backend;

import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;

public record ZLinkBackendActorLifecycleInfo(
    ZLinkBackendActorRef previousActor,
    ZLinkBackendActorRef currentActor,
    Optional<String> previousSpotId,
    Optional<String> currentSpotId,
    long joinEpoch,
    int flags) {
    public ZLinkBackendActorLifecycleInfo {
        previousSpotId = previousSpotId == null ? Optional.empty() : previousSpotId;
        currentSpotId = currentSpotId == null ? Optional.empty() : currentSpotId;
    }
}
