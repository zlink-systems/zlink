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
        // The DEALER has no blocking receive owner: the framework only asks
        // it for readiness with a zero timeout from the ClientServer control
        // tick. Claiming the completion queue for this poller would make the
        // binding deliver request replies and WRITABLE retries at that tick's
        // cadence — and not at all while the tick is blocked — so completion
        // ownership stays with the binding's context completion pump.
        this.receivePoller = new ZLinkJavaSocketReceivePoller(socket, false);
    }

    @Override public Socket nativeSocket() { return socket; }
    @Override public String name() { return "dealer"; }
    @Override public synchronized void bind(String endpoint) { socket.bind(endpoint); }
    @Override public synchronized void connect(String endpoint) { socket.connect(endpoint); }
    @Override public synchronized void disconnect(String endpoint) { socket.disconnect(endpoint); }
    @Override public void setChannelName(String channelName) { ZLinkJavaSocketSupport.validateChannelName(channelName); }
    @Override public void setReceiveFlowState(ReceiveFlowState state) {
        socket.options().receiveFlowState(state);
    }
    @Override public boolean waitForReadable(Duration timeout) {
        return receivePoller.waitForReadable(timeout);
    }

    @Override
    public synchronized CompletionStage<Void> send(List<Message> parts) {
        return ZLinkJavaSocketSupport.submit(socket.send(), parts);
    }

    @Override
    public synchronized CompletionStage<ZLinkBackendReceived> request(
        List<Message> parts,
        Duration timeout) {
        return ZLinkJavaSocketSupport.submitRequest(
            socket.request(), parts, timeout);
    }

    @Override
    public synchronized ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) {
        Received result = new Received();
        boolean transferred = false;
        try {
            if (!ZLinkJavaSocketSupport.recvOrNoData(
                    () -> socket.recv(result, ZLinkJavaSocketSupport.map(mode)))) {
                return null;
            }
            ZLinkBackendReceived received = ZLinkJavaSocketSupport.fromReceived(result);
            transferred = true;
            return received;
        } finally {
            if (!transferred) {
                result.close();
            }
        }
    }

    @Override
    public synchronized void close() {
        receivePoller.close();
        socket.close();
    }
}
