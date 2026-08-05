package systems.zlink.framework.runtime.internal.spots;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;

/** Internal bridge from the public fluent call to Instance Spot activation. */
public interface ZLinkInstanceSpotCallRuntime {
    default CompletionStage<Boolean> isStaleRoute(
        String spotId,
        SpotTransportAddress address) {
        return java.util.concurrent.CompletableFuture.completedFuture(false);
    }

    CompletionStage<Void> send(
        String spotId,
        String stableType,
        String meshName,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Map<String, String> metadata);

    CompletionStage<List<Message>> request(
        String spotId,
        String stableType,
        String meshName,
        Message payload,
        Optional<String> packetName,
        String contentType,
        Map<String, String> metadata,
        Duration timeout);
}
