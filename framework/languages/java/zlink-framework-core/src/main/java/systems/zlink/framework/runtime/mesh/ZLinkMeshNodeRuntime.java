package systems.zlink.framework.runtime.mesh;

import java.time.Duration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

public final class ZLinkMeshNodeRuntime implements AutoCloseable {
    private final ZLinkInternalMeshNode node;

    private ZLinkMeshNodeRuntime(ZLinkInternalMeshNode node) {
        this.node = node;
    }

    public static ZLinkMeshNodeRuntime start(
        MeshNodeRegistration registration,
        ZLinkMeshBackendAdapter adapter,
        ZLinkBackendContext context) {
        return start(registration, adapter, context, null);
    }

    static ZLinkMeshNodeRuntime start(
        MeshNodeRegistration registration,
        ZLinkMeshBackendAdapter adapter,
        ZLinkBackendContext context,
        ZLinkInboundDispatchBudget applicationDispatchBudget) {
        ZLinkInternalMeshNode node =
            adapter.createMeshNode(context, registration.meshName());
        boolean started = false;
        try {
            if (registration.routingId() != null) {
                node.setRoutingId(registration.routingId());
            }
            node.setBind(registration.bindEndpoint());
            node.setObjectRole(
                registration.objectServer()
                    ? systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.SERVER
                    : registration.objectRoleEnabled()
                        ? systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.CLIENT
                        : systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.NONE);
            node.setPlacementWeight(registration.placementWeight());
            //  The HWM is an accounted byte count and stays 64-bit. The pending
            //  admission capacity is a message count, so a byte HWM larger than
            //  int range saturates instead of wrapping.
            long routerSendHighWaterMark =
                registration.configureRouterSocket().sendHighWaterMark();
            node.setRouterHighWaterMark(routerSendHighWaterMark);
            node.setRouterPendingAdmissionCapacity(
                routerSendHighWaterMark > 0
                    ? (int) Math.min(routerSendHighWaterMark, Integer.MAX_VALUE)
                    : 4096);
            node.setRouterSendTimeout(
                registration.configureRouterSocket().sendTimeout()
                    .orElse(Duration.ofSeconds(1)));
            node.setMailboxMessageBudget(
                registration.configureRouterSocket().receiveHighWaterMark());
            if (applicationDispatchBudget != null) {
                node.setApplicationDispatchBudget(applicationDispatchBudget);
            }
            registration.channelWeights().forEach((channelName, weight) -> {
                node.addChannel(channelName);
                node.setChannelWeight(channelName, weight);
            });
            if (!registration.spotFactories().isEmpty()
                || !registration.entrySpots().isEmpty()
                || !registration.actorFactories().isEmpty()
                || !registration.channelNames().isEmpty()) {
                node.spotNode();
            }
            node.start();
            for (MeshNodeRegistration.Peer peer : registration.peers()) {
                if (peer.expectedRoutingId() == null) {
                    if (!registration.objectRoleEnabled()) {
                        node.connectPeer(peer.endpoint());
                    }
                } else {
                    node.connectPeer(peer.endpoint(), peer.expectedRoutingId());
                }
            }
            started = true;
            return new ZLinkMeshNodeRuntime(node);
        } finally {
            if (!started) {
                node.close();
            }
        }
    }

    public ZLinkInternalMeshNode node() {
        return node;
    }

    @Override
    public void close() {
        node.close();
    }
}
