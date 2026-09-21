package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;

import java.util.List;

public interface ZLinkBackendPublisherSocket extends ZLinkBackendSocket {
    void setChannelName(String channelName);

    void setRoutingId(RoutingId routingId);

    void setNoDrop(boolean noDrop);

    default String lastEndpoint() {
        return null;
    }

    boolean publish(String topic, List<Message> parts, SendFlags flags);
}
