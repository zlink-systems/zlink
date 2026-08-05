package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

@FunctionalInterface
public interface ZLinkBackendStreamPacketHandler {
    void handle(RoutingId routingId, Message header, Message payload);
}
