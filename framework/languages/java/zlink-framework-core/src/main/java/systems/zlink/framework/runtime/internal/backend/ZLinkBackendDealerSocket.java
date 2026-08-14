package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;

public interface ZLinkBackendDealerSocket
    extends ZLinkBackendConnectableSocket, ZLinkBackendReceiveSocket {
    void setChannelName(String channelName);

    CompletionStage<Void> send(List<Message> parts);

    CompletionStage<ZLinkBackendReceived> request(
        List<Message> parts,
        Duration timeout);

    ZLinkBackendReceived recv(ZLinkBackendRecvMode mode);
}
