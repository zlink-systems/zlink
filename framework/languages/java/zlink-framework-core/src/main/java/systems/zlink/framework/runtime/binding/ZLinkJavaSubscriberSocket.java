package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;

final class ZLinkJavaSubscriberSocket
    implements ZLinkBackendSubscriberSocket, ZLinkJavaSocketBacked {
    private final SubSocket socket;
    private final ZLinkJavaSocketReceivePoller receivePoller;

    ZLinkJavaSubscriberSocket(SubSocket socket) {
        this.socket = socket;
        this.receivePoller = new ZLinkJavaSocketReceivePoller(socket);
    }

    @Override public Socket nativeSocket() { return socket; }
    @Override public String name() { return "subscriber"; }
    @Override public synchronized void bind(String endpoint) { socket.bind(endpoint); }
    @Override public synchronized void connect(String endpoint) { socket.connect(endpoint); }
    @Override public synchronized void disconnect(String endpoint) { socket.disconnect(endpoint); }
    @Override public synchronized void setChannelName(String channelName) { ZLinkJavaSocketSupport.validateChannelName(channelName); }
    @Override public synchronized void setSubscription(String topic) { socket.setSubscription(topic); }
    @Override public boolean waitForReadable(Duration timeout) {
        return receivePoller.waitForReadable(timeout);
    }

    @Override
    public synchronized ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) {
        try (TopicMessage result = new TopicMessage()) {
            return socket.subscribe(result, ZLinkJavaSocketSupport.map(mode))
                ? new ZLinkBackendTopicMessage(
                    result.getRoutingId(),
                    result.topic(),
                    ZLinkJavaBackendCodec.copyParts(result.parts()))
                : null;
        }
    }

    @Override public synchronized void close() {
        receivePoller.close();
        socket.close();
    }
}
