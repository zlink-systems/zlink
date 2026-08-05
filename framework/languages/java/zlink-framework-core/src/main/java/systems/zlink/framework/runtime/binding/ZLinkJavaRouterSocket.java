package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;

final class ZLinkJavaRouterSocket
    implements ZLinkBackendRouterSocket, ZLinkJavaSocketBacked {
    private final RouterSocket socket;
    private final ZLinkJavaSocketReceivePoller receivePoller;

    ZLinkJavaRouterSocket(RouterSocket socket) {
        this.socket = socket;
        this.receivePoller = new ZLinkJavaSocketReceivePoller(socket);
    }

    @Override public Socket nativeSocket() { return socket; }
    @Override public String name() { return "router"; }
    @Override public synchronized void bind(String endpoint) { socket.bind(endpoint); }
    @Override public synchronized void connect(String endpoint) { socket.connect(endpoint); }
    @Override public synchronized void disconnect(String endpoint) { socket.disconnect(endpoint); }
    @Override public synchronized void setChannelName(String channelName) { ZLinkJavaSocketSupport.validateChannelName(channelName); }
    @Override public synchronized void setRoutingId(RoutingId routingId) { socket.setRoutingId(routingId); }
    @Override public synchronized void setConnectRoutingId(RoutingId routingId) { socket.options().setConnectRoutingId(routingId); }
    @Override public synchronized void setProbe(boolean enabled) { socket.options().probe(enabled); }
    @Override public synchronized long maxMessageSize() { return Math.max(0, socket.options().maxMessageSize()); }
    @Override public synchronized void setMaxMessageSize(long value) { socket.options().maxMessageSize(value == 0 ? -1 : value); }
    @Override public synchronized int peerWeight() { return socket.options().peerWeight(); }
    @Override public synchronized void setPeerWeight(int weight) { socket.options().peerWeight(weight); }
    @Override public synchronized String lastEndpoint() { return socket.options().lastEndpoint(); }
    @Override public boolean waitForReadable(Duration timeout) {
        return receivePoller.waitForReadable(timeout);
    }

    @Override
    public synchronized ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) {
        try (Received result = new Received()) {
            return ZLinkJavaSocketSupport.recvOrNoData(
                    () -> socket.recv(result, ZLinkJavaSocketSupport.map(mode)))
                ? ZLinkJavaSocketSupport.fromReceived(result)
                : null;
        }
    }

    @Override
    public synchronized boolean send(RoutingId routingId, List<Message> parts, SendFlags flags) {
        return ZLinkJavaSocketSupport.submit(socket.send(routingId), parts, flags);
    }

    @Override
    public synchronized boolean request(
        RoutingId routingId,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        return ZLinkJavaSocketSupport.submitRequest(
            socket.request(routingId),
            parts,
            callback,
            flags,
            timeout);
    }

    @Override
    public synchronized void reply(RoutingId routingId, long requestSeq, List<Message> parts) {
        ZLinkJavaSocketSupport.submitReply(socket.reply(routingId, requestSeq), parts);
    }

    @Override public synchronized void disconnectPeer(RoutingId routingId) {
        socket.disconnectRid(routingId);
    }

    @Override public synchronized void close() {
        notifyAdmissionShutdown();
        receivePoller.close();
        socket.close();
    }
}
