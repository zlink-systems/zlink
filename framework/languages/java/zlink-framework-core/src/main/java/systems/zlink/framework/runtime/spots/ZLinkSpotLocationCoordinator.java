package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.spots.ZLinkSpotKind;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkSpotLocationCoordinator {
    private final Map<RoutingId, NodeLocation> nodes = new HashMap<>();
    private ZLinkLocationLifecycle lifecycle;

    void registerNode(
            String meshName,
            RoutingId nodeRid,
            String entrySpotId,
            long entrySpotGeneration,
            String routeEndpoint,
            boolean publisherEnabled) {
        nodes.put(
                nodeRid,
                new NodeLocation(
                        meshName,
                        nodeRid,
                        entrySpotId,
                        entrySpotGeneration,
                        routeEndpoint,
                        publisherEnabled));
    }

    void setLifecycle(ZLinkLocationLifecycle lifecycle) {
        this.lifecycle = lifecycle;
    }

    String publisherChannelName(RoutingId nodeRid) {
        NodeLocation node = nodes.get(nodeRid);
        return node != null && node.publisherEnabled() ? node.meshName() : null;
    }

    String meshName(RoutingId nodeRid) {
        NodeLocation node = nodes.get(nodeRid);
        return node == null ? null : node.meshName();
    }

    String entrySpotMeshName(String spotId) {
        if (spotId == null) {
            return null;
        }
        return nodes.values().stream()
                .filter(candidate -> candidate.entrySpotId().equals(spotId))
                .findFirst()
                .map(NodeLocation::meshName)
                .orElse(null);
    }

    CompletionStage<Void> claimEntrySpotsAsync() {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (NodeLocation node : nodes.values()) {
            chain =
                    chain.thenCompose(
                            ignored -> claimEntrySpotAsync(node).thenApply(status -> null));
        }
        return chain;
    }

    CompletionStage<Void> releaseUserSpotAsync(RoutingId nodeRid, String spotId) {
        if (lifecycle == null) {
            return CompletableFuture.completedFuture(null);
        }
        NodeLocation node = nodes.get(nodeRid);
        if (node == null) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.releaseSpot(node.meshName(), spotId);
    }

    CompletionStage<Void> releaseEntrySpotAsync(RoutingId nodeRid) {
        NodeLocation node = nodes.get(nodeRid);
        if (lifecycle == null || node == null || !node.hasRouteEndpoint()) {
            return CompletableFuture.completedFuture(null);
        }
        return lifecycle.releaseSpot(node.meshName(), node.entrySpotId());
    }

    private CompletionStage<ZLinkLocationWriteStatus> claimEntrySpotAsync(NodeLocation node) {
        if (!node.hasRouteEndpoint() || lifecycle == null) {
            return CompletableFuture.completedFuture(ZLinkLocationWriteStatus.STORED);
        }
        return lifecycle.claimSpot(
                node.meshName(),
                node.entrySpotId(),
                node.entrySpotGeneration(),
                null,
                node.nodeRid(),
                ZLinkSpotKind.ENTRY,
                node.routeEndpoint(),
                null);
    }

    private record NodeLocation(
            String meshName,
            RoutingId nodeRid,
            String entrySpotId,
            long entrySpotGeneration,
            String routeEndpoint,
            boolean publisherEnabled) {

        boolean hasRouteEndpoint() {
            return routeEndpoint != null && !routeEndpoint.isBlank();
        }
    }
}
