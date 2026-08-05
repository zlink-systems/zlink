package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;

public interface ZLinkBackendSpotRouteBridge extends ZLinkBackendObject {
    void attachRouterChannel(
        String channelName,
        ZLinkBackendRouterSocket router);

    boolean send(
        String channelName,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> parts,
        SendFlags flags);

    boolean request(
        String channelName,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout);

    default CompletionStage<List<Message>> requestAsync(
        String channelName,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> parts,
        SendFlags flags,
        Duration timeout) {
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        try {
            boolean submitted = request(
                channelName,
                targetNodeRid,
                targetSpotId,
                parts,
                reply -> {
                    if (reply.result() == ZLinkBackendRequestResult.OK) {
                        result.complete(reply.parts());
                    } else {
                        reply.parts().forEach(Message::close);
                        result.completeExceptionally(new IllegalStateException(
                            "SPOT route bridge request failed: " + reply.result()));
                    }
                },
                flags,
                timeout);
            if (!submitted) {
                result.completeExceptionally(new IllegalStateException(
                    "SPOT route bridge request was not submitted"));
            }
        } catch (RuntimeException ex) {
            result.completeExceptionally(ex);
        }
        return result;
    }

    boolean handleRouterReceived(
        String channelName,
        RoutingId sourceNodeRid,
        long requestSeq,
        List<Message> parts);

    int drain();

    void close();
}
