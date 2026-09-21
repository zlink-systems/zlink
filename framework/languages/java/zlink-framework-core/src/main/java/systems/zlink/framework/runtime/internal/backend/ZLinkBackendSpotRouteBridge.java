package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;

public interface ZLinkBackendSpotRouteBridge extends ZLinkBackendObject {
    void attachRouterChannel(String channelName, ZLinkBackendRouterSocket router);

    CompletionStage<Void> send(
            String channelName, RoutingId targetNodeRid, String targetSpotId, List<Message> parts);

    CompletionStage<List<Message>> request(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            Duration timeout);

    boolean handleRouterReceived(
            String channelName, RoutingId sourceNodeRid, long requestSeq, List<Message> parts);

    int drain();

    void close();
}
