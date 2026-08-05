package systems.zlink.framework.runtime.binding;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;

record ZLinkJavaPublisherSocket(PubSocket socket)
    implements ZLinkBackendPublisherSocket, ZLinkJavaSocketBacked {
    @Override public Socket nativeSocket() { return socket; }
    @Override public String name() { return "publisher"; }
    @Override public void bind(String endpoint) { socket.bind(endpoint); }
    @Override public void setChannelName(String channelName) { ZLinkJavaSocketSupport.validateChannelName(channelName); }
    @Override public void setRoutingId(RoutingId routingId) { socket.setRoutingId(routingId); }
    @Override public String lastEndpoint() { return socket.options().lastEndpoint(); }
    @Override public boolean publish(String topic, List<Message> parts, SendFlags flags) {
        return ZLinkJavaSocketSupport.submit(socket.publish(topic), parts, flags);
    }
    @Override public void close() {
        notifyAdmissionShutdown();
        socket.close();
    }
}
