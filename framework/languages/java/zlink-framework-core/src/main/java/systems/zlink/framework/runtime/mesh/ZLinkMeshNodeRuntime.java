package systems.zlink.framework.runtime.mesh;

import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.function.Consumer;

/** Configures and starts one MeshNode that {@link ZLinkMeshNodesRuntime} already owns. */
final class ZLinkMeshNodeRuntime {
    private ZLinkMeshNodeRuntime() {}

    static void start(
            ZLinkInternalMeshNode node,
            MeshNodeRegistration registration,
            boolean deferServiceReadyPublication,
            Consumer<ZLinkMeshDispatchRecord> receiver,
            ZLinkApplicationJobQueue applicationJobQueue) {
        if (applicationJobQueue != null) {
            node.setApplicationJobQueue(applicationJobQueue);
        }
        if (registration.routingId() != null) {
            node.setRoutingId(registration.routingId());
        }
        node.setBind(registration.bindEndpoint());
        node.setAdvertiseHost(registration.advertiseHost());
        node.setObjectRole(
                registration.objectServer()
                        ? ZLinkMeshNodeObjectRole.SERVER
                        : registration.objectRoleEnabled()
                                ? ZLinkMeshNodeObjectRole.CLIENT
                                : ZLinkMeshNodeObjectRole.NONE);
        node.setPlacementWeight(registration.placementWeight());
        node.setRouterSocketConfig(registration.routerSocketConfig());
        registration
                .configureRouterSocket()
                .receiveTimeout()
                .ifPresent(node::setRouterReceiveTimeout);
        if (receiver instanceof ZLinkMeshApplicationReceiver applicationReceiver) {
            node.setApplicationReceiver(applicationReceiver);
        }
        if (receiver != null) {
            node.startDispatch(receiver);
        }
        registration
                .channelWeights()
                .forEach(
                        (channelName, weight) -> {
                            node.addChannel(channelName);
                            node.setChannelWeight(channelName, weight);
                        });
        if (!registration.spotFactories().isEmpty()
                || !registration.entrySpots().isEmpty()
                || !registration.actorFactories().isEmpty()
                || !registration.channelNames().isEmpty()) {
            node.spotNode();
        }
        if (deferServiceReadyPublication) {
            node.deferServiceReadyPublication();
        }
        node.start();
        for (MeshNodeRegistration.Peer peer : registration.peers()) {
            if (peer.expectedRoutingId() == null) {
                node.connectPeer(peer.endpoint());
            } else {
                node.connectPeer(peer.endpoint(), peer.expectedRoutingId());
            }
        }
    }
}
