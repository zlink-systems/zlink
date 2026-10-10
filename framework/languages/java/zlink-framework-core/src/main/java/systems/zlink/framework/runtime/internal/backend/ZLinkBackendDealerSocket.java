package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.ReceiveFlowState;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;

public interface ZLinkBackendDealerSocket
        extends ZLinkBackendConnectableSocket, ZLinkBackendReceiveSocket {
    void setChannelName(String channelName);

    CompletionStage<Void> send(List<Message> parts);

    CompletionStage<ZLinkBackendReceived> request(List<Message> parts, Duration timeout);

    Runnable receiveAdmission();

    void setReceiveAdmission(Runnable admission);

    default CompletionStage<ZLinkBackendReceived> request(
            List<Message> parts, Duration timeout, Runnable admission) {
        return request(parts, timeout)
                .whenComplete(
                        (reply, failure) -> {
                            if (failure == null && admission != null) admission.run();
                        });
    }

    ZLinkBackendReceived recv(ZLinkBackendRecvMode mode);

    /** Applies the host's absolute paired-socket receive-flow state. */
    default void setReceiveFlowState(ReceiveFlowState state) {
        throw new UnsupportedOperationException(
                "paired dealer receive-flow control is not available");
    }
}
