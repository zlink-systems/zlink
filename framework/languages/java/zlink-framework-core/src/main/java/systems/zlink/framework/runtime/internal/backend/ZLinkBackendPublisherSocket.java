package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;

public interface ZLinkBackendPublisherSocket extends ZLinkBackendSocket {
    void setChannelName(String channelName);

    void setRoutingId(RoutingId routingId);

    default String lastEndpoint() {
        return null;
    }

    boolean publish(String topic, List<Message> parts, SendFlags flags);
}
