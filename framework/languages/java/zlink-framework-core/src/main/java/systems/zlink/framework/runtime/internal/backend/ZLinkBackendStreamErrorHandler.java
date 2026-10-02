package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.MonitorEventType;

public interface ZLinkBackendStreamErrorHandler {
    void handle(RoutingId routingId, MonitorEventType event, int nativeCode, String message);
}
