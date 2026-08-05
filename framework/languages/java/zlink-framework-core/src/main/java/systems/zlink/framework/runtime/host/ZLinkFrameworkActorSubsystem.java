package systems.zlink.framework.runtime.host;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.actors.ZLinkActorClientRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorEntrySpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkActorEntryTransferEnvelope;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkStoreActorDirectory;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkFrameworkActorSubsystem {
    private final ZLinkActorRuntime actors;
    private final ZLinkActorDirectory actorDirectory;
    private final ZLinkActorClient actorClient;

    private ZLinkFrameworkActorSubsystem(
        ZLinkActorRuntime actors,
        ZLinkActorDirectory actorDirectory,
        ZLinkActorClient actorClient) {
        this.actors = actors;
        this.actorDirectory = actorDirectory;
        this.actorClient = actorClient;
    }

    static ZLinkFrameworkActorSubsystem create(
        systems.zlink.framework.runtime.internal.backend
            .ZLinkBackendAdapterProvider backendFactory,
        ZLinkFrameworkRegistration registration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator.MutableServices runtimeHandlers,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        ZLinkStreamCodec defaultStreamCodec,
        ZLinkChannelRuntime channels,
        ZLinkSpotRuntime spots,
        ZLinkLocationLifecycle locationLifecycle,
        ZLinkStoreLocationResolvers storeLocationResolvers,
        SpotTransportAddressResolver remoteAddressResolver,
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locationStore,
        java.util.Map<String,
            systems.zlink.framework.runtime.internal.backend
                .ZLinkInternalMeshNode> meshNodes,
        java.util.concurrent.CompletionStage<Void> runtimeReady) {
        var legacyActorNode = registration.spotNodes().stream()
            .filter(node -> !node.actorFactories().isEmpty())
            .findFirst()
            .orElse(null);
        var meshActorNode = registration.meshNodes().stream()
            .filter(node -> !node.actorFactories().isEmpty())
            .findFirst()
            .orElseGet(() -> registration.meshNodes().stream()
                .filter(node -> node.objectRoleEnabled())
                .findFirst()
                .orElse(null));
        String actorNodeName = legacyActorNode != null
            ? legacyActorNode.nodeName()
            : meshActorNode == null ? null : meshActorNode.meshName();
        var actorFactories = legacyActorNode != null
            ? legacyActorNode.actorFactories()
            : meshActorNode == null ? java.util.Map.<String,
                Class<? extends systems.zlink.framework.actors.ZLinkActorFactory>>of()
                : meshActorNode.actorFactories();
        java.util.Map<String, Class<? extends
            systems.zlink.framework.actors.ZLinkActorRelocationAdapter<?>>>
            transferAdapters = new java.util.LinkedHashMap<>();
        if (meshActorNode != null) {
            meshActorNode.relocatableActorFactories().forEach(
                (stableType, factory) -> {
                    if (factory.relocationPolicy()
                        instanceof systems.zlink.framework.runtime.internal
                            .configuration.ZLinkObjectFactoryRegistration
                            .RelocationPolicy.PreserveState preserveState) {
                        @SuppressWarnings("unchecked")
                        Class<? extends systems.zlink.framework.actors
                            .ZLinkActorRelocationAdapter<?>> adapter =
                            (Class<? extends systems.zlink.framework.actors
                                .ZLinkActorRelocationAdapter<?>>)
                                preserveState.adapterClass();
                        transferAdapters.put(stableType, adapter);
                    }
                });
        }
        ZLinkActorRuntime actors = spots != null && actorNodeName != null
            ? new ZLinkActorRuntime(
                spots.node(actorNodeName),
                actorFactories,
                transferAdapters,
                registration.defaultRequestTimeout(),
                registration.messageFollowDuration(),
                serializer,
                runtimeHandlers,
                defaultStreamCodec,
                ZLinkAdmissionRuntime.factory(backendFactory),
                registration.serialExecutor())
            : null;
        ZLinkActorDirectory actorDirectory = actors != null
            ? actors
            : spots != null && storeLocationResolvers != null
                ? new ZLinkStoreActorDirectory(storeLocationResolvers)
                : null;
        if (actors != null) {
            actors.setMeshName(actorNodeName);
            actors.setMetadataPolicy(
                registration.metadataPolicy().sessionToActorKeys(),
                registration.metadataPolicy().actorToSessionKeys());
            if (meshActorNode != null && locationStore != null) {
                var meshNode = meshNodes.get(meshActorNode.meshName());
                if (meshNode != null) {
                    var creation =
                        new systems.zlink.framework.runtime.actors
                            .ZLinkActorCreationCoordinator(
                                meshActorNode.meshName(),
                                meshNode,
                                locationStore,
                                actors,
                                serializer);
                    meshNode.setActorCreateOperationHandler(creation);
                    actors.setCreationSubmitter(creation);
                    actors.setEntrySpotTargetSelector(
                        creation::selectEntrySpotTarget);
                }
            }
        }
        ZLinkActorClient actorClient = spots != null && storeLocationResolvers != null
            ? new ZLinkActorClientRuntime(
                spots::primaryNode,
                storeLocationResolvers,
                serializer,
                registration.defaultRequestTimeout(),
                ZLinkAdmissionRuntime.factory(backendFactory),
                runtimeReady)
            : null;
        registerActorServices(runtimeHandlers, actorClient, actorDirectory, actors);
        wireActorRuntime(
            registration,
            handlerFactory,
            eventDispatcher,
            channels,
            spots,
            locationLifecycle,
            storeLocationResolvers,
            remoteAddressResolver,
            meshNodes,
            actors);
        return new ZLinkFrameworkActorSubsystem(actors, actorDirectory, actorClient);
    }

    ZLinkActorRuntime actors() {
        return actors;
    }

    ZLinkActorDirectory actorDirectory() {
        return actorDirectory;
    }

    ZLinkActorClient actorClient() {
        return actorClient;
    }

    private static void registerActorServices(
        ZLinkHandlerActivator.MutableServices runtimeHandlers,
        ZLinkActorClient actorClient,
        ZLinkActorDirectory actorDirectory,
        ZLinkActorRuntime actors) {
        if (actorClient != null) {
            runtimeHandlers.add(ZLinkActorClient.class, actorClient);
        }
        if (actorDirectory != null) {
            runtimeHandlers.add(ZLinkActorDirectory.class, actorDirectory);
        }
        if (actors != null) {
            runtimeHandlers.add(ZLinkActorManager.class, actors);
        }
    }

    private static void wireActorRuntime(
        ZLinkFrameworkRegistration registration,
        ZLinkHandlerActivator handlerFactory,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        ZLinkChannelRuntime channels,
        ZLinkSpotRuntime spots,
        ZLinkLocationLifecycle locationLifecycle,
        ZLinkStoreLocationResolvers storeLocationResolvers,
        SpotTransportAddressResolver remoteAddressResolver,
        java.util.Map<String,
            systems.zlink.framework.runtime.internal.backend
                .ZLinkInternalMeshNode> meshNodes,
        ZLinkActorRuntime actors) {
        if (actors == null) {
            return;
        }

        var meshActorNode = registration.meshNodes().stream()
            .filter(node -> !node.actorFactories().isEmpty())
            .findFirst()
            .orElseGet(() -> registration.meshNodes().stream()
                .filter(node -> node.objectRoleEnabled())
                .findFirst()
                .orElse(null));
        actors.setLocationLifecycle(locationLifecycle);
        actors.setStoreLocationResolvers(storeLocationResolvers);
        meshNodes.values().forEach(node ->
            node.setApplicationStreamCodecResolver(
                registration.codecs()::streamCodecForReceivedContentType));
        if (meshActorNode != null) {
            var meshNode = meshNodes.get(meshActorNode.meshName());
            if (meshNode != null) {
                actors.setMessageFollowNoticeSender(meshNode::sendMessageFollow);
                try {
                    meshNode.spotNode().setMessageFollowRelayHandler(
                        actors::relayMessageFollow);
                } catch (UnsupportedOperationException ignored) {
                    // Alternate backends can expose actor runtime without the
                    // raw M6B service relay boundary.
                }
            }
        }
        if (storeLocationResolvers != null) {
            meshNodes.values().forEach(node -> node.setMessageFollowHandler(
                (sourceNodeRid, notice) -> {
                    if (!node.status().routingId().equals(
                        notice.source().targetNodeRid())
                        || !sourceNodeRid.equals(
                            notice.target().targetNodeRid())) {
                        return;
                    }
                    storeLocationResolvers.invalidateRouteIfMatches(notice);
                }));
        }
        actors.setMessageFlowTracer(new ZLinkMessageFlowTracer(
            registration.dispatchOptions(),
            handlerFactory,
            registration.handlerExecutor(),
            eventDispatcher));
        actors.setRoutedTransport(channels, () -> spots.primaryNode().entrySpot().spotId());
        if (remoteAddressResolver != null) {
            actors.setRemoteAddressResolver(remoteAddressResolver);
        }
        spots.attachActorRuntime(actors);
        channels.registerRouteInternalRequestHandler(
            ZLinkActorEntrySpotRoutePackets.JOIN_ENTRY_SPOT_PACKET_NAME,
            actors::handleEntrySpotRouteJoin);
        channels.registerRouteInternalRequestHandler(
            ZLinkActorEntryTransferEnvelope.PACKET_NAME,
            spots::handleEntryActorTransferRoute);
    }
}
