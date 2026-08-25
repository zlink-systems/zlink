package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;

final class ZLinkJavaDealerSocket
    implements ZLinkBackendDealerSocket, ZLinkJavaSocketBacked {
    private final DealerSocket socket;
    private final ZLinkJavaSocketReceivePoller receivePoller;

    ZLinkJavaDealerSocket(DealerSocket socket) {
        this.socket = socket;
        this.receivePoller = new ZLinkJavaSocketReceivePoller(socket);
    }

    @Override public Socket nativeSocket() { return socket; }
    @Override public String name() { return "dealer"; }
    @Override public synchronized void bind(String endpoint) { socket.bind(endpoint); }
    @Override public synchronized void connect(String endpoint) { socket.connect(endpoint); }
    @Override public synchronized void disconnect(String endpoint) { socket.disconnect(endpoint); }
    @Override public synchronized void setChannelName(String channelName) { ZLinkJavaSocketSupport.validateChannelName(channelName); }
    @Override public synchronized void setReceiveFlowState(ReceiveFlowState state) {
        socket.options().receiveFlowState(state);
    }
    @Override public boolean waitForReadable(Duration timeout) {
        return receivePoller.waitForReadable(timeout);
    }

    @Override
    public synchronized CompletionStage<Void> send(List<Message> parts) {
        synchronized (socket) {
            return ZLinkJavaSocketSupport.submit(socket.send(), parts);
        }
    }

    @Override
    public synchronized CompletionStage<ZLinkBackendReceived> request(
        List<Message> parts,
        Duration timeout) {
        synchronized (socket) {
            return ZLinkJavaSocketSupport.submitRequest(
                socket.request(), parts, timeout);
        }
    }

    @Override
    public synchronized ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) {
        synchronized (socket) {
            try (Received result = new Received()) {
                return ZLinkJavaSocketSupport.recvOrNoData(
                        () -> socket.recv(result, ZLinkJavaSocketSupport.map(mode)))
                    ? ZLinkJavaSocketSupport.fromReceived(result)
                    : null;
            }
        }
    }

    @Override
    public synchronized void close() {
        synchronized (socket) {
            receivePoller.close();
            socket.close();
        }
    }
}
