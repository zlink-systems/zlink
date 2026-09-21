package systems.zlink.framework.runtime.internal.spots;

import systems.zlink.contracts.messaging.Message;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Internal bridge from the public fluent call to Instance Spot activation. */
public interface ZLinkInstanceSpotCallRuntime {
    default String metricMeshName(String requestedMesh, String callerMesh) {
        return callerMesh;
    }

    default CompletionStage<Boolean> isStaleRoute(String spotId, SpotTransportAddress address) {
        return CompletableFuture.completedFuture(false);
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
