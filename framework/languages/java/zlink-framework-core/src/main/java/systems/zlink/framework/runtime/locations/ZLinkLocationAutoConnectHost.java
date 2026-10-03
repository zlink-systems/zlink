package systems.zlink.framework.runtime.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendConnectableSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkPeerIntentClosePendingException;
import systems.zlink.framework.runtime.internal.channels.ZLinkClientServerRuntimeConfiguration;
import systems.zlink.framework.runtime.internal.channels.ZLinkFanoutRuntimeConfiguration;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeerResolver;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.spots.SpotNodeRegistration;

import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.stream.Collectors;

public final class ZLinkLocationAutoConnectHost implements AutoCloseable {
    public static final String SPOT_PUB_ENDPOINT_METADATA_KEY = "pub-endpoint";

    private final ZLinkLocationRuntime runtime;
    private final ZLinkRuntimeEventDispatcher runtimeEvents;
    private final ZLinkAutoConnectPeerResolver peers;
    private final ZLinkLocationOptions options;
    private final ZLinkClientServerRuntimeConfiguration clientServers;
    private final ZLinkFanoutRuntimeConfiguration fanout;
    private final List<ZLinkAutoConnectLoop> loops = new ArrayList<>();
    private volatile boolean clientServersStarted;
    private volatile boolean fanoutStarted;

    public ZLinkLocationAutoConnectHost(
            ZLinkLocationRuntime runtime,
            ZLinkAutoConnectPeerResolver peers,
            ZLinkLocationOptions options,
            ZLinkClientServerRuntimeConfiguration clientServers,
            ZLinkFanoutRuntimeConfiguration fanout,
            ZLinkRuntimeEventDispatcher runtimeEvents) {
        this.runtimeEvents = Objects.requireNonNull(runtimeEvents, "runtimeEvents");
        this.runtime = Objects.requireNonNull(runtime, "runtime");
        this.peers = Objects.requireNonNull(peers, "peers");
        this.options = Objects.requireNonNull(options, "options");
        this.clientServers = clientServers;
        this.fanout = fanout;
    }

    public CompletionStage<Void> start(
            ZLinkFrameworkRegistration registration,
            ZLinkChannelRuntime channels,
            Map<String, ZLinkInternalMeshNode> meshNodesByName,
            Map<String, ZLinkInternalSpotNode> spotNodesByName) {
        Objects.requireNonNull(registration, "registration");
        Objects.requireNonNull(channels, "channels");
        Objects.requireNonNull(meshNodesByName, "meshNodesByName");
        Objects.requireNonNull(spotNodesByName, "spotNodesByName");

        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces = channels.autoConnectSurfaces();
        boolean hasAutomaticClientServer =
                surfaces.stream()
                        .anyMatch(surface -> surface.type() == ZLinkAutoConnectType.CLIENT_SERVER);
        if (hasAutomaticClientServer && (clientServers == null || clientServers.store() == null)) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "automatic ClientServer channels require "
                                    + "ZLinkLocationRepository with ClientServer descriptors"));
        }
        boolean hasAutomaticFanoutSubscriber =
                surfaces.stream()
                        .anyMatch(
                                surface ->
                                        surface.type() == ZLinkAutoConnectType.FANOUT
                                                && surface.role() == ZLinkLocationRole.SUB);
        boolean hasFanoutPublisher =
                surfaces.stream()
                        .anyMatch(
                                surface ->
                                        surface.type() == ZLinkAutoConnectType.FANOUT
                                                && surface.role() == ZLinkLocationRole.PUB);
        boolean hasAutomaticFanout =
                hasAutomaticFanoutSubscriber
                        || (hasFanoutPublisher && fanout != null && fanout.store() != null);
        if (hasAutomaticFanoutSubscriber && (fanout == null || fanout.store() == null)) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "automatic classic fanout requires "
                                    + "ZLinkLocationRepository with fanout descriptors"));
        }
        for (ZLinkChannelRuntime.AutoConnectSurface surface : surfaces) {
            if (surface.type() == ZLinkAutoConnectType.CLIENT_SERVER
                    || surface.type() == ZLinkAutoConnectType.FANOUT) {
                continue;
            }
            addChannelLoop(surface);
        }
        for (MeshNodeRegistration mesh : registration.meshNodes()) {
            ZLinkInternalMeshNode node = meshNodesByName.get(mesh.meshName());
            if (node == null) {
                continue;
            }
            String endpoint = node.advertisedEndpoint();
            Set<String> manual =
                    mesh.peers().stream()
                            .map(MeshNodeRegistration.Peer::endpoint)
                            .collect(Collectors.toUnmodifiableSet());
            Map<String, RoutingId> manualExpectedRids = new HashMap<>();
            mesh.peers()
                    .forEach(
                            peer ->
                                    manualExpectedRids.put(
                                            peer.endpoint(), peer.expectedRoutingId()));
            addLoop(
                    ZLinkAutoConnectType.ROUTE_MESH,
                    mesh.meshName(),
                    ZLinkLocationRole.ROUTER,
                    mesh.routingId(),
                    endpoint,
                    100,
                    new MeshNodeExecutor(node, manual, manualExpectedRids),
                    null,
                    null,
                    mesh.objectServer()
                            ? ZLinkMeshNodeObjectRole.SERVER
                            : mesh.objectRoleEnabled()
                                    ? ZLinkMeshNodeObjectRole.CLIENT
                                    : ZLinkMeshNodeObjectRole.NONE,
                    !mesh.channelWeights().isEmpty());
        }
        for (SpotNodeRegistration spot : registration.spotNodes()) {
            ZLinkInternalSpotNode node = spotNodesByName.get(spot.nodeName());
            if (node == null || !spot.routerEnabled()) {
                continue;
            }
            Map<String, String> metadata = new LinkedHashMap<>();
            if (spot.pubBind() != null) {
                metadata.put(SPOT_PUB_ENDPOINT_METADATA_KEY, spot.pubBind());
            }
            List<String> capabilities = actorCapabilities(spot.actorFactories().keySet());
            Set<String> manual =
                    spot.routerManualConnections().stream()
                            .map(SpotNodeRegistration.RouterManualConnection::endpoint)
                            .collect(Collectors.toUnmodifiableSet());
            addLoop(
                    ZLinkAutoConnectType.SPOT_MESH,
                    spot.meshName(),
                    ZLinkLocationRole.SPOT,
                    node.routingId(),
                    spot.routerBind() == null ? "" : spot.routerBind(),
                    100,
                    new SpotNodeExecutor(node, manual),
                    metadata.isEmpty() ? null : Map.copyOf(metadata),
                    capabilities.isEmpty() ? null : capabilities,
                    ZLinkMeshNodeObjectRole.NONE,
                    false);
        }

        CompletionStage<Void> chain =
                hasAutomaticClientServer
                        ? startClientServers().thenRun(() -> clientServersStarted = true)
                        : CompletableFuture.completedFuture(null);
        if (hasAutomaticFanout) {
            chain = chain.thenCompose(ignored -> startFanout().thenRun(() -> fanoutStarted = true));
        }
        for (ZLinkAutoConnectLoop loop : loops) {
            chain = chain.thenCompose(ignored -> loop.start());
        }
        return chain;
    }

    public CompletionStage<Void> stop() {
        CompletionStage<Void> chain =
                clientServers == null
                        ? CompletableFuture.completedFuture(null)
                        : clientServers.stop();
        if (fanout != null) {
            chain = chain.thenCompose(ignored -> fanout.stop());
        }
        for (ZLinkAutoConnectLoop loop : loops) {
            chain = chain.thenCompose(ignored -> loop.stop());
        }
        return chain.whenComplete(
                (ignored, failure) -> {
                    loops.clear();
                });
    }

    static List<String> actorCapabilities(Collection<String> actorTypes) {
        return actorTypes.stream()
                .map(actorType -> "actor:" + actorType)
                .distinct()
                .sorted()
                .toList();
    }

    private CompletionStage<Void> startClientServers() {
        clientServers.setOwner(runtime.ownerTokenSnapshot());
        return clientServers.start();
    }

    private CompletionStage<Void> startFanout() {
        fanout.setOwner(runtime.ownerTokenSnapshot());
        return fanout.start();
    }

    public CompletionStage<Void> recoverOwnerLease() {
        var owner = runtime.ownerTokenSnapshot();
        if (clientServers != null && clientServers.store() != null) {
            clientServers.setOwner(owner);
        }
        if (fanout != null && fanout.store() != null) {
            fanout.setOwner(owner);
        }
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> markDraining() {
        CompletionStage<Void> chain =
                !clientServersStarted
                        ? CompletableFuture.completedFuture(null)
                        : clientServers.markDraining();
        if (fanoutStarted) {
            chain = chain.thenCompose(ignored -> fanout.markDraining());
        }
        for (ZLinkAutoConnectLoop loop : loops) {
            chain = chain.thenCompose(ignored -> loop.markDraining());
        }
        return chain;
    }

    private void addChannelLoop(ZLinkChannelRuntime.AutoConnectSurface surface) {
        ZLinkAutoConnectExecutor executor =
                surface.socket() == null
                        ? ZLinkAutoConnectExecutor.NONE
                        : new ConnectableSocketExecutor(
                                surface.socket(), Set.copyOf(surface.manualEndpoints()));
        if (surface.socket() instanceof ZLinkBackendRouterSocket router
                && surface.type() == ZLinkAutoConnectType.ROUTE_MESH) {
            executor = new RouteSocketExecutor(router, Set.copyOf(surface.manualEndpoints()));
        }
        addLoop(
                surface.type(),
                surface.meshName(),
                surface.role(),
                surface.nodeRid(),
                surface.endpoint(),
                surface.weight(),
                executor,
                null,
                null,
                ZLinkMeshNodeObjectRole.NONE,
                false);
    }

    private void addLoop(
            ZLinkAutoConnectType type,
            String meshName,
            ZLinkLocationRole role,
            RoutingId nodeRid,
            String endpoint,
            long weight,
            ZLinkAutoConnectExecutor executor,
            Map<String, String> metadata,
            List<String> capabilities,
            ZLinkMeshNodeObjectRole objectRole,
            boolean hasRouteMeshServerChannel) {
        boolean advertisable = shouldAdvertise(type, role, nodeRid, endpoint);
        if (!advertisable && executor == ZLinkAutoConnectExecutor.NONE) {
            return;
        }
        ZLinkAutoConnectPlanner.Local local =
                new ZLinkAutoConnectPlanner.Local(
                        type,
                        meshName,
                        role,
                        nodeRid,
                        endpoint,
                        objectRole,
                        hasRouteMeshServerChannel);
        ZLinkAutoConnectReconciler reconciler =
                new ZLinkAutoConnectReconciler(
                        local,
                        peers,
                        executor,
                        options,
                        System::nanoTime,
                        failure ->
                                runtimeEvents.publishRuntimeTaskFailure("auto-connect", failure));
        loops.add(new ZLinkAutoConnectLoop(reconciler, options));
    }

    static boolean shouldAdvertise(
            ZLinkAutoConnectType type, ZLinkLocationRole role, RoutingId nodeRid, String endpoint) {
        boolean hasEndpoint = endpoint != null && !endpoint.isBlank();
        return switch (type) {
            case CLIENT_SERVER -> role == ZLinkLocationRole.ROUTER && hasEndpoint;
            case FANOUT -> role == ZLinkLocationRole.PUB && hasEndpoint;
            default -> ZLinkAutoConnectPlanner.hasRid(nodeRid) || hasEndpoint;
        };
    }

    @Override
    public void close() {
        stop();
    }

    private static final class ConnectableSocketExecutor implements ZLinkAutoConnectExecutor {
        private final ZLinkBackendConnectableSocket socket;
        private final Set<String> manualEndpoints;

        ConnectableSocketExecutor(
                ZLinkBackendConnectableSocket socket, Set<String> manualEndpoints) {
            this.socket = socket;
            this.manualEndpoints = manualEndpoints;
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            if (!manualEndpoints.contains(target.endpoint())) {
                socket.connect(target.endpoint());
            }
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            if (!manualEndpoints.contains(target.endpoint())) {
                socket.disconnect(target.endpoint());
            }
            return true;
        }
    }

    private static final class RouteSocketExecutor implements ZLinkAutoConnectExecutor {
        private final ZLinkBackendRouterSocket socket;
        private final Set<String> manualEndpoints;

        RouteSocketExecutor(ZLinkBackendRouterSocket socket, Set<String> manualEndpoints) {
            this.socket = socket;
            this.manualEndpoints = manualEndpoints;
        }

        @Override
        public boolean isManual(ZLinkAutoConnectPlanner.Target target) {
            return manualEndpoints.contains(target.endpoint());
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            if (manualEndpoints.contains(target.endpoint())) {
                return true;
            }
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                socket.setConnectRoutingId(target.nodeRid());
                socket.setProbe(true);
                socket.connect(target.endpoint());
                return true;
            }
            socket.connect(target.endpoint());
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            if (!manualEndpoints.contains(target.endpoint())) {
                socket.disconnect(target.endpoint());
            }
            return true;
        }

        @Override
        public boolean replace(
                ZLinkAutoConnectPlanner.Target current,
                ZLinkAutoConnectPlanner.Target replacement) {
            socket.disconnect(current.endpoint());
            connectReplacement(replacement);
            return true;
        }

        private void connectReplacement(ZLinkAutoConnectPlanner.Target target) {
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                socket.setConnectRoutingId(target.nodeRid());
                socket.setProbe(true);
                socket.connect(target.endpoint());
                return;
            }
            socket.connect(target.endpoint());
        }
    }

    private static String admissionSecurityIdentity(ZLinkAutoConnectPlanner.Target target) {
        String identity =
                target.metadata()
                        .getOrDefault(
                                ZLinkAutoConnectPlanner.SECURITY_IDENTITY_METADATA_KEY,
                                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
        // Older store rows say "plaintext", but current RouteMesh
        // descriptors encode the canonical wire placeholder "default".
        // This is a transport-mode label, not an authenticated identity.
        return "plaintext".equals(identity)
                ? ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY
                : identity;
    }

    static final class MeshNodeExecutor implements ZLinkAutoConnectExecutor {
        private final ZLinkInternalMeshNode node;
        private final Set<String> manualEndpoints;
        private final Map<String, RoutingId> manualExpectedRids;
        private final Map<String, ConnectionIntent> connectionIntents = new HashMap<>();

        MeshNodeExecutor(
                ZLinkInternalMeshNode node,
                Set<String> manualEndpoints,
                Map<String, RoutingId> manualExpectedRids) {
            this.node = node;
            this.manualEndpoints = manualEndpoints;
            this.manualExpectedRids = manualExpectedRids;
        }

        @Override
        public boolean isManual(ZLinkAutoConnectPlanner.Target target) {
            return manualEndpoints.contains(target.endpoint());
        }

        @Override
        public void observeAdmissionExpectation(ZLinkAutoConnectPlanner.Target target) {
            node.observePeerAdmissionExpectation(
                    target.nodeRid(),
                    target.endpoint(),
                    target.lifecycleGeneration(),
                    admissionSecurityIdentity(target));
        }

        @Override
        public void forgetAdmissionExpectation(ZLinkAutoConnectPlanner.Target target) {
            node.forgetPeerAdmissionExpectation(target.nodeRid());
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            boolean manual = manualEndpoints.contains(target.endpoint());
            if (manual && !ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                return true;
            }
            long intent;
            try {
                intent =
                        ZLinkAutoConnectPlanner.hasRid(target.nodeRid())
                                ? node.replacePeerConnection(
                                        target.endpoint(),
                                        target.nodeRid(),
                                        target.lifecycleGeneration(),
                                        admissionSecurityIdentity(target))
                                : node.connectPeer(target.endpoint());
            } catch (ZLinkPeerIntentClosePendingException pending) {
                //  The previous intent's close is still in progress; the next reconcile
                //  submits this connection again.
                return false;
            }
            connectionIntents.put(target.endpoint(), new ConnectionIntent(target.key(), intent));
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            ConnectionIntent current = connectionIntents.get(target.endpoint());
            if (current == null || !current.targetKey().equals(target.key())) {
                return true;
            }
            if (manualEndpoints.contains(target.endpoint())) {
                RoutingId fallbackRid = manualExpectedRids.get(target.endpoint());
                try {
                    node.replacePeerConnection(
                            target.endpoint(),
                            fallbackRid,
                            0,
                            fallbackRid == null ? null : fallbackRid.toString());
                } catch (ZLinkPeerIntentClosePendingException pending) {
                    return false;
                }
            } else {
                node.removePeerConnection(current.intentId());
            }
            connectionIntents.remove(target.endpoint(), current);
            return true;
        }

        @Override
        public void markNotRequired(ZLinkAutoConnectPlanner.Target target) {
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                node.markPeerConnectionNotRequired(
                        target.nodeRid(), target.endpoint(), target.lifecycleGeneration());
            }
        }

        @Override
        public void clearNotRequired(ZLinkAutoConnectPlanner.Target target) {
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                node.clearPeerConnectionNotRequired(target.nodeRid());
            }
        }

        private record ConnectionIntent(String targetKey, long intentId) {}
    }

    private static final class SpotNodeExecutor implements ZLinkAutoConnectExecutor {
        private final ZLinkInternalSpotNode node;
        private final Set<String> manualEndpoints;

        SpotNodeExecutor(ZLinkInternalSpotNode node, Set<String> manualEndpoints) {
            this.node = node;
            this.manualEndpoints = manualEndpoints;
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            if (manualEndpoints.contains(target.endpoint())) {
                return true;
            }
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                node.connectPeer(target.nodeRid(), target.endpoint());
            } else {
                node.connectPeer(target.endpoint());
            }
            String pubEndpoint = pubEndpointOf(target);
            if (pubEndpoint != null) {
                node.connectPeer(pubEndpoint);
            }
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            if (manualEndpoints.contains(target.endpoint())) {
                return true;
            }
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                node.disconnectPeer(target.nodeRid());
            } else {
                node.disconnectPeer(target.endpoint());
            }
            String pubEndpoint = pubEndpointOf(target);
            if (pubEndpoint != null) {
                node.disconnectPeer(pubEndpoint);
            }
            return true;
        }

        private static String pubEndpointOf(ZLinkAutoConnectPlanner.Target target) {
            if (target.metadata() == null) {
                return null;
            }
            String endpoint = target.metadata().get(SPOT_PUB_ENDPOINT_METADATA_KEY);
            return endpoint == null || endpoint.isBlank() ? null : endpoint;
        }
    }
}
