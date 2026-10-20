package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;

/**
 * Session-Actor binding §4: a test host whose live MeshNode descriptor publishes an owner token
 * that differs from its RID, as a Location Store host does.
 */
public final class ZLinkTestSessionOwners {
    private ZLinkTestSessionOwners() {}

    public static ZLinkInternalMeshNode.PeerAuthorityFence token(
            RoutingId nodeRid, long nodeGeneration) {
        return new ZLinkInternalMeshNode.PeerAuthorityFence(
                nodeRid, nodeGeneration, "owner-of-" + nodeRid, 1);
    }

    /** Installs the descriptor source on a started node and publishes its own owner fence. */
    public static ZLinkJavaRawMeshNode install(ZLinkJavaRawMeshNode node) {
        node.setPeerAuthorityResolver(
                (meshName, nodeRid, nodeGeneration) ->
                        CompletableFuture.completedFuture(
                                Optional.of(token(nodeRid, nodeGeneration))));
        node.refreshLocalAuthorityFence().toCompletableFuture().join();
        return node;
    }
}
