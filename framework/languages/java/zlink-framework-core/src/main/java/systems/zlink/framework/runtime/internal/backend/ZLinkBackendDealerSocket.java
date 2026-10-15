package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.messaging.Message;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkBackendDealerSocket
        extends ZLinkBackendConnectableSocket, ZLinkBackendReceiveSocket {
    void setChannelName(String channelName);

    CompletionStage<Void> send(List<Message> parts);

    Runnable receiveAdmission();

    void setReceiveAdmission(Runnable admission);

    ZLinkBackendReceived recv(ZLinkBackendRecvMode mode);

    CompletionStage<ZLinkBackendReceived> request(
            List<Message> parts,
            Duration timeout,
            Runnable receiveAdmission,
            CompletableFuture<Void> requestAdmission);
}
