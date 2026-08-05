package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;

public interface ZLinkBackendStreamErrorHandler {
    void handle(RoutingId routingId, int nativeCode, String message);
}
