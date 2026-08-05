package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;
import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;

public interface ZLinkBackendRouterSocket
    extends ZLinkBackendConnectableSocket, ZLinkBackendReceiveSocket {
    void setChannelName(String channelName);

    void setRoutingId(RoutingId routingId);

    void setConnectRoutingId(RoutingId routingId);

    void setProbe(boolean enabled);

    long maxMessageSize();

    void setMaxMessageSize(long value);

    int peerWeight();

    void setPeerWeight(int weight);

    default String lastEndpoint() {
        return "";
    }

    ZLinkBackendReceived recv(ZLinkBackendRecvMode mode);

    boolean send(RoutingId routingId, List<Message> parts, SendFlags flags);

    boolean request(
        RoutingId routingId,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout);

    void reply(RoutingId routingId, long requestSeq, List<Message> parts);

    default void disconnectPeer(RoutingId routingId) {
        throw new UnsupportedOperationException(
            "router peer disconnect is not available");
    }
}
