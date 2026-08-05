package systems.zlink.framework.runtime.configuration;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.channels.ChannelRegistration;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.locations.ZLinkLocationRegistration;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.spots.SpotNodeRegistration;
import systems.zlink.framework.runtime.streams.StreamNodeRegistration;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodecs;

public final class ZLinkFrameworkRegistration {
    private final ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
    private final ZLinkMetadataPolicyRegistration metadataPolicy =
        new ZLinkMetadataPolicyRegistration();
    private final ZLinkDispatchOptionsRegistration dispatchOptions =
        new ZLinkDispatchOptionsRegistration();
    private final ZLinkWorkerOptionsRegistration workers =
        new ZLinkWorkerOptionsRegistration();
    private final ZLinkLocationRegistration locations = new ZLinkLocationRegistration();
    private final ZLinkInboundDispatchRegistration inboundDispatch =
        new ZLinkInboundDispatchRegistration();
    private volatile ZLinkInboundDispatchBudget inboundDispatchBudget;
    private final List<ChannelRegistration> channels = new ArrayList<>();
    private final List<MeshNodeRegistration> meshNodes = new ArrayList<>();
    private final List<SpotNodeRegistration> spotNodes = new ArrayList<>();
    private final List<StreamNodeRegistration> streamNodes = new ArrayList<>();
    private final Set<Class<?>> handlerPackageMarkers = new LinkedHashSet<>();
    private final List<Class<? extends ZLinkHandlerFilter>> filters = new ArrayList<>();
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers = new ArrayList<>();
    private ZLinkStreamCompressionCodec streamCompressionCodec = ZLinkStreamCompressionCodecs.lz4();
    private Executor handlerExecutor = Executors.newVirtualThreadPerTaskExecutor();
    private boolean closeHandlerExecutor = true;
    private final ExecutorService serialExecutor =
        Executors.newVirtualThreadPerTaskExecutor();
    private Duration defaultRequestTimeout = Duration.ofSeconds(30);
    private Duration messageFollowDuration = Duration.ofSeconds(30);
    private ZLinkRelocationStore relocationStore;
    private long applicationVersion;
    private String maintenanceWave;

    public Duration defaultRequestTimeout() {
        return defaultRequestTimeout;
    }

    void setDefaultRequestTimeout(Duration defaultRequestTimeout) {
        this.defaultRequestTimeout = defaultRequestTimeout;
    }

    public Duration messageFollowDuration() {
        return messageFollowDuration;
    }

    public long applicationVersion() {
        return applicationVersion;
    }

    void setApplicationVersion(long applicationVersion) {
        this.applicationVersion = applicationVersion;
    }

    public java.util.Optional<String> maintenanceWave() {
        return java.util.Optional.ofNullable(maintenanceWave);
    }

    void setMaintenanceWave(String maintenanceWave) {
        this.maintenanceWave = maintenanceWave;
    }

    public ZLinkCodecRegistration codecs() {
        return codecs;
    }

    public ZLinkMetadataPolicyRegistration metadataPolicy() {
        return metadataPolicy;
    }

    public ZLinkDispatchOptionsRegistration dispatchOptions() {
        return dispatchOptions;
    }

    public ZLinkWorkerOptionsRegistration workers() {
        return workers;
    }

    public ZLinkLocationRegistration locations() {
        return locations;
    }

    public ZLinkInboundDispatchRegistration inboundDispatch() {
        return inboundDispatch;
    }

    /** Returns the process-wide inbound dispatch budget after configuration validation. */
    public synchronized ZLinkInboundDispatchBudget inboundDispatchBudget() {
        if (inboundDispatchBudget == null) {
            inboundDispatchBudget = new ZLinkInboundDispatchBudget(
                ZLinkInboundDispatchBudget.resolveApplicationHwm(inboundDispatch));
        }
        return inboundDispatchBudget;
    }

    public List<ChannelRegistration> channels() {
        return channels;
    }

    public List<MeshNodeRegistration> meshNodes() {
        return meshNodes;
    }

    public List<SpotNodeRegistration> spotNodes() {
        return spotNodes;
    }

    public List<StreamNodeRegistration> streamNodes() {
        return streamNodes;
    }

    public Set<Class<?>> handlerPackageMarkers() {
        return handlerPackageMarkers;
    }

    public List<Class<? extends ZLinkHandlerFilter>> filters() {
        return filters;
    }

    public List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers() {
        return List.copyOf(suspendHandlerInvokers);
    }

    public ZLinkStreamCompressionCodec streamCompressionCodec() {
        return streamCompressionCodec;
    }

    public void useStreamCompression(ZLinkStreamCompressionCodec codec) {
        streamCompressionCodec = codec;
    }

    public Executor handlerExecutor() {
        return handlerExecutor;
    }

    public boolean closeHandlerExecutor() {
        return closeHandlerExecutor;
    }

    /** Executor owned by the framework runtime for serial queue turns. */
    public ExecutorService serialExecutor() {
        return serialExecutor;
    }

    public Set<Class<?>> applicationTypes() {
        Set<Class<?>> types = new LinkedHashSet<>();
        types.addAll(filters);
        for (ChannelRegistration channel : channels) {
            types.addAll(channel.handlerTypes());
        }
        for (MeshNodeRegistration meshNode : meshNodes) {
            types.addAll(meshNode.applicationTypes());
        }
        for (SpotNodeRegistration spotNode : spotNodes) {
            types.addAll(spotNode.spotFactories());
            types.addAll(spotNode.entrySpots());
            types.addAll(spotNode.actorFactories().values());
        }
        for (StreamNodeRegistration streamNode : streamNodes) {
            types.addAll(streamNode.applicationTypes());
        }
        for (ZLinkScannedHandler handler : ZLinkHandlerScanner.scan(handlerPackageMarkers).handlers()) {
            types.add(handler.handlerType());
        }
        return Set.copyOf(types);
    }

    void setLocationStore(ZLinkLocationStore store) {
        locations.setStoreInstance(store);
    }

    public ZLinkRelocationStore relocationStore() {
        return relocationStore;
    }

    void setRelocationStore(
        systems.zlink.framework.locationprovider.ZLinkRelocationStore store) {
        if (relocationStore != null) {
            throw new ZLinkConfigurationException(
                "relocation store is already registered");
        }
        relocationStore =
            new systems.zlink.framework.runtime.internal.locations
                .ZLinkProviderRelocationRepository(store);
    }

    void useVirtualThreadHandlers() {
        closeOwnedHandlerExecutor();
        handlerExecutor = Executors.newVirtualThreadPerTaskExecutor();
        closeHandlerExecutor = true;
    }

    void useHandlerExecutor(Executor executor) {
        if (executor == null) {
            throw new ZLinkConfigurationException("handler executor is required");
        }
        closeOwnedHandlerExecutor();
        handlerExecutor = executor;
        closeHandlerExecutor = false;
    }

    void useSuspendHandlerInvoker(ZLinkSuspendInvocationAdapter invoker) {
        if (invoker == null) {
            throw new ZLinkConfigurationException("suspend handler invoker is required");
        }
        suspendHandlerInvokers.clear();
        suspendHandlerInvokers.add(invoker);
    }

    void validate() {
        dispatchOptions.validate();
        workers.validate();
        long applicationHwmBytes =
            ZLinkInboundDispatchBudget.resolveApplicationHwm(inboundDispatch);
        ZLinkScannedHandlerCatalog handlerCatalog =
            ZLinkHandlerScanner.scan(handlerPackageMarkers);
        for (ChannelRegistration channel : channels) {
            channel.validate(locations.enabled(), handlerCatalog);
            channel.validateApplicationHwm(applicationHwmBytes);
        }
        Set<String> clientServerChannelNames = channels.stream()
            .filter(channel -> channel.kind()
                == systems.zlink.framework.runtime.channels.ChannelKind.CLIENT_SERVER)
            .map(ChannelRegistration::name)
            .collect(java.util.stream.Collectors.toSet());
        int actorCapableNodes = 0;
        boolean objectRoleConfigured = false;
        boolean relocationStoreRequired = false;
        for (MeshNodeRegistration meshNode : meshNodes) {
            meshNode.validate();
            meshNode.validateApplicationHwm(applicationHwmBytes);
            for (String channelName : meshNode.channelNames()) {
                if (clientServerChannelNames.contains(channelName)) {
                    throw new ZLinkConfigurationException(
                        "ChannelName '" + channelName
                            + "' is registered on both RouteMesh and ClientServer physical paths.");
                }
            }
            objectRoleConfigured |= meshNode.objectRoleEnabled();
            if (meshNode.objectRoleEnabled()
                && !meshNode.objectServer()
                && !meshNode.nodeHandlers().isEmpty()) {
                throw new ZLinkConfigurationException(
                    "Object Client cannot register application Node direct handlers: "
                        + meshNode.meshName());
            }
            relocationStoreRequired |= meshNode.requiresRelocationStore();
            if (!meshNode.actorFactories().isEmpty()) {
                actorCapableNodes++;
            }
        }
        for (SpotNodeRegistration spotNode : spotNodes) {
            spotNode.validate();
            if (!spotNode.actorFactories().isEmpty()) {
                actorCapableNodes++;
            }
        }
        if (actorCapableNodes > 1) {
            throw new ZLinkConfigurationException(
                "actor factory registration is ambiguous because more than one mesh node owns actor factories");
        }
        if (objectRoleConfigured && !locations.enabled()) {
            throw new ZLinkConfigurationException(
                "Mesh object Client or Server role requires a Location Store");
        }
        if (relocationStoreRequired && relocationStore == null) {
            throw new ZLinkConfigurationException(
                "Recreate or Snapshot relocation policy requires a Relocation Store");
        }
        for (StreamNodeRegistration streamNode : streamNodes) {
            streamNode.validate(meshNodes);
            streamNode.validateApplicationHwm(applicationHwmBytes);
        }
    }

    private void closeOwnedHandlerExecutor() {
        if (closeHandlerExecutor && handlerExecutor instanceof AutoCloseable closeable) {
            try {
                closeable.close();
            } catch (Exception ex) {
                throw new ZLinkConfigurationException("failed to close handler executor", ex);
            }
        }
    }
}
