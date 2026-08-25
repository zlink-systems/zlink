package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.ReceiveFlowState;

public interface ZLinkBackendDealerSocket
    extends ZLinkBackendConnectableSocket, ZLinkBackendReceiveSocket {
    void setChannelName(String channelName);

    CompletionStage<Void> send(List<Message> parts);

    CompletionStage<ZLinkBackendReceived> request(
        List<Message> parts,
        Duration timeout);

    ZLinkBackendReceived recv(ZLinkBackendRecvMode mode);

    /** Applies the host's absolute paired-socket receive-flow state. */
    default void setReceiveFlowState(ReceiveFlowState state) {
        throw new UnsupportedOperationException(
            "paired dealer receive-flow control is not available");
    }
}
