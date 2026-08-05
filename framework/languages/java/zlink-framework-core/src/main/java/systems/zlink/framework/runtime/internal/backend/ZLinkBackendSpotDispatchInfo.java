package systems.zlink.framework.runtime.internal.backend;

import java.util.List;

public record ZLinkBackendSpotDispatchInfo(
    ZLinkBackendSpotDispatchEvent event,
    List<ZLinkBackendActorReceived> actorMessages) {
    public ZLinkBackendSpotDispatchInfo {
        actorMessages = List.copyOf(actorMessages);
    }
}
