package systems.zlink.framework.runtime.locations;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeerResolver;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendConnectableSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.internal.channels.ZLinkClientServerRuntimeConfiguration;
import systems.zlink.framework.runtime.internal.channels.ZLinkFanoutRuntimeConfiguration;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.runtime.spots.SpotNodeRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

public final class ZLinkLocationAutoConnectHost implements AutoCloseable {
    public static final String SPOT_PUB_ENDPOINT_METADATA_KEY = "pub-endpoint";

    private final ZLinkLocationRuntime runtime;
    private final ZLinkAutoConnectPeerResolver peers;
    private final ZLinkLocationOptions options;
    private final ZLinkClientServerRuntimeConfiguration clientServers;
    private final ZLinkFanoutRuntimeConfiguration fanout;
    private final List<ZLinkAutoConnectLoop> loops = new ArrayList<>();

    public ZLinkLocationAutoConnectHost(
        ZLinkLocationRuntime runtime,
        ZLinkAutoConnectPeerResolver peers,
        ZLinkLocationOptions options) {
        this(runtime, peers, options, null, null);
    }

    public ZLinkLocationAutoConnectHost(
        ZLinkLocationRuntime runtime,
        ZLinkAutoConnectPeerResolver peers,
        ZLinkLocationOptions options,
        ZLinkClientServerRuntimeConfiguration clientServers) {
        this(runtime, peers, options, clientServers, null);
    }

    public ZLinkLocationAutoConnectHost(
        ZLinkLocationRuntime runtime,
        ZLinkAutoConnectPeerResolver peers,
        ZLinkLocationOptions options,
        ZLinkClientServerRuntimeConfiguration clientServers,
        ZLinkFanoutRuntimeConfiguration fanout) {
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
        Map<String, ZLinkInternalSpotNode> spotNodesByName,
        ZLinkSpotRuntime spots) {
        Objects.requireNonNull(registration, "registration");
        Objects.requireNonNull(channels, "channels");
        Objects.requireNonNull(meshNodesByName, "meshNodesByName");
        Objects.requireNonNull(spotNodesByName, "spotNodesByName");

        List<ZLinkChannelRuntime.AutoConnectSurface> surfaces = channels.autoConnectSurfaces();
        boolean hasAutomaticClientServer = surfaces.stream().anyMatch(
            surface -> surface.type() == ZLinkAutoConnectType.CLIENT_SERVER);
        if (hasAutomaticClientServer
            && (clientServers == null || clientServers.store() == null)) {
            return CompletableFuture.failedFuture(
                new systems.zlink.framework.errors.ZLinkConfigurationException(
                    "automatic ClientServer channels require "
                        + "ZLinkLocationRepository with ClientServer descriptors"));
        }
        boolean hasAutomaticFanoutSubscriber = surfaces.stream().anyMatch(
            surface -> surface.type() == ZLinkAutoConnectType.FANOUT
                && surface.role() == ZLinkLocationRole.SUB);
        boolean hasFanoutPublisher = surfaces.stream().anyMatch(
            surface -> surface.type() == ZLinkAutoConnectType.FANOUT
                && surface.role() == ZLinkLocationRole.PUB);
        boolean hasAutomaticFanout = hasAutomaticFanoutSubscriber
            || (hasFanoutPublisher
                && fanout != null
                && fanout.store() != null);
        if (hasAutomaticFanoutSubscriber
            && (fanout == null || fanout.store() == null)) {
            return CompletableFuture.failedFuture(
                new systems.zlink.framework.errors.ZLinkConfigurationException(
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
            String endpoint = node.status().localEndpoint();
            if (endpoint == null || endpoint.isBlank()) {
                endpoint = mesh.bindEndpoint();
            }
            endpoint = mesh.advertisedEndpoint(endpoint);
            Set<String> manual = mesh.peers().stream()
                .map(MeshNodeRegistration.Peer::endpoint)
                .collect(java.util.stream.Collectors.toUnmodifiableSet());
            addLoop(
                ZLinkAutoConnectType.ROUTE_MESH,
                mesh.meshName(),
                ZLinkLocationRole.ROUTER,
                mesh.routingId(),
                endpoint,
                100,
                new MeshNodeExecutor(node, manual, spots),
                null,
                null,
                mesh.objectServer()
                    ? systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.SERVER
                    : mesh.objectRoleEnabled()
                        ? systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.CLIENT
                        : systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.NONE,
                !mesh.channelWeights().isEmpty());
        }
        for (SpotNodeRegistration spot : registration.spotNodes()) {
            ZLinkInternalSpotNode node = spotNodesByName.get(spot.nodeName());
            if (node == null || !spot.routerEnabled()) {
                continue;
            }
            Map<String, String> metadata = new java.util.LinkedHashMap<>();
            if (spot.pubBind() != null) {
                metadata.put(SPOT_PUB_ENDPOINT_METADATA_KEY, spot.pubBind());
            }
            List<String> capabilities = actorCapabilities(spot.actorFactories().keySet());
            Set<String> manual = spot.routerManualConnections().stream()
                .map(SpotNodeRegistration.RouterManualConnection::endpoint)
                .collect(java.util.stream.Collectors.toUnmodifiableSet());
            addLoop(
                ZLinkAutoConnectType.SPOT_MESH,
                spot.meshName(),
                ZLinkLocationRole.SPOT,
                node.routingId(),
                spot.routerBind() == null ? "" : spot.routerBind(),
                100,
                new SpotNodeExecutor(node, manual, spots),
                metadata.isEmpty() ? null : Map.copyOf(metadata),
                capabilities.isEmpty() ? null : capabilities,
                systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.NONE,
                false);
        }

        CompletionStage<Void> chain = hasAutomaticClientServer
            ? startClientServers()
            : CompletableFuture.completedFuture(null);
        if (hasAutomaticFanout) {
            chain = chain.thenCompose(ignored -> startFanout());
        }
        for (ZLinkAutoConnectLoop loop : loops) {
            chain = chain.thenCompose(ignored -> loop.start());
        }
        return chain;
    }

    public CompletionStage<Void> stop() {
        CompletionStage<Void> chain = clientServers == null
            ? CompletableFuture.completedFuture(null)
            : clientServers.stop();
        if (fanout != null) {
            chain = chain.thenCompose(ignored -> fanout.stop());
        }
        for (ZLinkAutoConnectLoop loop : loops) {
            chain = chain.thenCompose(ignored -> loop.stop());
        }
        return chain.whenComplete((ignored, failure) -> {
            loops.clear();
        });
    }

    static List<String> actorCapabilities(java.util.Collection<String> actorTypes) {
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

    public CompletionStage<Void> markDraining() {
        CompletionStage<Void> chain = clientServers == null
            ? CompletableFuture.completedFuture(null)
            : clientServers.markDraining();
        if (fanout != null) {
            chain = chain.thenCompose(ignored -> fanout.markDraining());
        }
        for (ZLinkAutoConnectLoop loop : loops) {
            chain = chain.thenCompose(ignored -> loop.markDraining());
        }
        return chain;
    }

    private void addChannelLoop(ZLinkChannelRuntime.AutoConnectSurface surface) {
        ZLinkAutoConnectExecutor executor = surface.socket() == null
            ? ZLinkAutoConnectExecutor.NONE
            : new ConnectableSocketExecutor(surface.socket(), Set.copyOf(surface.manualEndpoints()));
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
            systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.NONE,
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
        systems.zlink.framework.locations.ZLinkMeshNodeObjectRole objectRole,
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
        ZLinkAutoConnectReconciler reconciler = new ZLinkAutoConnectReconciler(
            local,
            null,
            runtime,
            peers,
            executor,
            options);
        loops.add(new ZLinkAutoConnectLoop(reconciler, options));
    }

    static boolean shouldAdvertise(
        ZLinkAutoConnectType type,
        ZLinkLocationRole role,
        RoutingId nodeRid,
        String endpoint) {
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
            ZLinkBackendConnectableSocket socket,
            Set<String> manualEndpoints) {
            this.socket = socket;
            this.manualEndpoints = manualEndpoints;
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            if (!manualEndpoints.contains(target.endpoint())) {
                try {
                    socket.connect(target.endpoint());
                } catch (RuntimeException failure) {
                    return false;
                }
            }
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            if (!manualEndpoints.contains(target.endpoint())) {
                try {
                    socket.disconnect(target.endpoint());
                } catch (RuntimeException failure) {
                    return false;
                }
            }
            return true;
        }

    }

    private static final class RouteSocketExecutor implements ZLinkAutoConnectExecutor {
        private final ZLinkBackendRouterSocket socket;
        private final Set<String> manualEndpoints;
        private final Object connectGate = new Object();

        RouteSocketExecutor(
            ZLinkBackendRouterSocket socket,
            Set<String> manualEndpoints) {
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
            try {
                if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                    synchronized (connectGate) {
                        socket.setConnectRoutingId(target.nodeRid());
                        socket.setProbe(true);
                        socket.connect(target.endpoint());
                    }
                    return true;
                }
                socket.connect(target.endpoint());
                return true;
            } catch (RuntimeException failure) {
                return false;
            }
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            if (!manualEndpoints.contains(target.endpoint())) {
                try {
                    socket.disconnect(target.endpoint());
                } catch (RuntimeException failure) {
                    return false;
                }
            }
            return true;
        }

        @Override
        public boolean replace(
            ZLinkAutoConnectPlanner.Target current,
            ZLinkAutoConnectPlanner.Target replacement) {
            try {
                socket.disconnect(current.endpoint());
                connectReplacement(replacement);
                return true;
            } catch (RuntimeException failure) {
                return false;
            }
        }

        private void connectReplacement(ZLinkAutoConnectPlanner.Target target) {
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                synchronized (connectGate) {
                    socket.setConnectRoutingId(target.nodeRid());
                    socket.setProbe(true);
                    socket.connect(target.endpoint());
                }
                return;
            }
            socket.connect(target.endpoint());
        }

    }

    private static final class MeshNodeExecutor implements ZLinkAutoConnectExecutor {
        private final ZLinkInternalMeshNode node;
        private final Set<String> manualEndpoints;
        private final ZLinkSpotRuntime spots;
        private final Map<String, Long> connectionIntents =
            new java.util.concurrent.ConcurrentHashMap<>();
        private final Map<String, Long> connectionAttemptNanos =
            new java.util.concurrent.ConcurrentHashMap<>();
        private static final long ADMISSION_RETRY_NANOS =
            java.time.Duration.ofSeconds(1).toNanos();

        MeshNodeExecutor(
            ZLinkInternalMeshNode node,
            Set<String> manualEndpoints,
            ZLinkSpotRuntime spots) {
            this.node = node;
            this.manualEndpoints = manualEndpoints;
            this.spots = spots;
        }

        @Override
        public boolean isManual(ZLinkAutoConnectPlanner.Target target) {
            return manualEndpoints.contains(target.endpoint());
        }

        @Override
        public void observeAdmissionExpectation(
            ZLinkAutoConnectPlanner.Target target) {
            node.observePeerAdmissionExpectation(
                target.nodeRid(),
                target.endpoint(),
                target.lifecycleGeneration(),
                target.metadata().getOrDefault(
                    ZLinkAutoConnectPlanner
                        .SECURITY_IDENTITY_METADATA_KEY,
                    target.nodeRid().toString()));
        }

        @Override
        public void forgetAdmissionExpectation(
            ZLinkAutoConnectPlanner.Target target) {
            node.forgetPeerAdmissionExpectation(target.nodeRid());
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            if (manualEndpoints.contains(target.endpoint())) {
                return true;
            }
            try {
                long intent = ZLinkAutoConnectPlanner.hasRid(target.nodeRid())
                    ? node.connectPeer(
                        target.endpoint(),
                        target.nodeRid(),
                        target.lifecycleGeneration(),
                        target.metadata().getOrDefault(
                            ZLinkAutoConnectPlanner
                                .SECURITY_IDENTITY_METADATA_KEY,
                            target.nodeRid().toString()))
                    : node.connectPeer(target.endpoint());
                connectionIntents.put(target.key(), intent);
                connectionAttemptNanos.put(
                    target.key(),
                    System.nanoTime());
                if (spots != null && ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                    spots.markAutoConnectedRouterPeer(target.nodeRid());
                }
                return true;
            } catch (RuntimeException failure) {
                return false;
            }
        }

        @Override
        public void ensureConnected(ZLinkAutoConnectPlanner.Target target) {
            boolean admitted = node.peers().stream().anyMatch(peer ->
                peer.routingId().equals(target.nodeRid())
                    && peer.lifecycleGeneration()
                        == target.lifecycleGeneration()
                    && peer.state()
                        == systems.zlink.framework.runtime.internal.binding
                            .spot.MeshPeerState.ADMITTED);
            if (admitted) {
                return;
            }
            long attemptedAt = connectionAttemptNanos.getOrDefault(
                target.key(),
                0L);
            if (System.nanoTime() - attemptedAt < ADMISSION_RETRY_NANOS) {
                return;
            }
            Long previous = connectionIntents.remove(target.key());
            if (previous != null) {
                try {
                    node.removePeerConnection(previous);
                } catch (RuntimeException ignored) {
                    connectionIntents.putIfAbsent(target.key(), previous);
                    return;
                }
            }
            connect(target);
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            if (manualEndpoints.contains(target.endpoint())) {
                return true;
            }
            Long intent = connectionIntents.remove(target.key());
            connectionAttemptNanos.remove(target.key());
            if (intent == null) {
                return true;
            }
            try {
                node.removePeerConnection(intent);
                if (spots != null && ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                    spots.unmarkAutoConnectedRouterPeer(target.nodeRid());
                }
                return true;
            } catch (RuntimeException failure) {
                connectionIntents.putIfAbsent(target.key(), intent);
                return false;
            }
        }

        @Override
        public void markNotRequired(ZLinkAutoConnectPlanner.Target target) {
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                node.markPeerConnectionNotRequired(
                    target.nodeRid(),
                    target.endpoint(),
                    target.lifecycleGeneration());
            }
        }

        @Override
        public void clearNotRequired(ZLinkAutoConnectPlanner.Target target) {
            if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                node.clearPeerConnectionNotRequired(target.nodeRid());
            }
        }
    }

    private static final class SpotNodeExecutor implements ZLinkAutoConnectExecutor {
        private final ZLinkInternalSpotNode node;
        private final Set<String> manualEndpoints;
        private final ZLinkSpotRuntime spots;

        SpotNodeExecutor(
            ZLinkInternalSpotNode node,
            Set<String> manualEndpoints,
            ZLinkSpotRuntime spots) {
            this.node = node;
            this.manualEndpoints = manualEndpoints;
            this.spots = spots;
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            if (manualEndpoints.contains(target.endpoint())) {
                return true;
            }
            try {
                if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                    node.connectPeer(target.nodeRid(), target.endpoint());
                    if (spots != null) {
                        spots.markAutoConnectedRouterPeer(target.nodeRid());
                    }
                } else {
                    node.connectPeer(target.endpoint());
                }
                String pubEndpoint = pubEndpointOf(target);
                if (pubEndpoint != null) {
                    node.connectPeer(pubEndpoint);
                }
                return true;
            } catch (RuntimeException failure) {
                return false;
            }
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            if (manualEndpoints.contains(target.endpoint())) {
                return true;
            }
            try {
                if (ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                    node.disconnectPeer(target.nodeRid());
                    if (spots != null) {
                        spots.unmarkAutoConnectedRouterPeer(target.nodeRid());
                    }
                } else {
                    node.disconnectPeer(target.endpoint());
                }
                String pubEndpoint = pubEndpointOf(target);
                if (pubEndpoint != null) {
                    node.disconnectPeer(pubEndpoint);
                }
                return true;
            } catch (RuntimeException failure) {
                return false;
            }
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
