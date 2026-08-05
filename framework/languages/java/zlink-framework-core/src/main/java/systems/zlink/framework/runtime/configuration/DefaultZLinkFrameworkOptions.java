package systems.zlink.framework.runtime.configuration;

import java.time.Duration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.Executor;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.configuration.ClientServerChannelBuilder;
import systems.zlink.framework.configuration.FanoutChannelBuilder;
import systems.zlink.framework.runtime.internal.configuration.RouteMeshChannelBuilder;
import systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder;
import systems.zlink.framework.configuration.ZLinkDispatchOptions;
import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder;
import systems.zlink.framework.configuration.ZLinkNetworkOptions;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.runtime.internal.configuration.ZLinkSpotMeshBuilder;
import systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder;
import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder;
import systems.zlink.framework.configuration.ZLinkWorkerOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkRelocationStore;
import systems.zlink.framework.runtime.channels.ChannelBuilders;
import systems.zlink.framework.runtime.channels.ChannelKind;
import systems.zlink.framework.runtime.channels.ChannelRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.spots.SpotBuilders;
import systems.zlink.framework.runtime.spots.SpotNodeRegistration;
import systems.zlink.framework.runtime.streams.StreamBuilders;
import systems.zlink.framework.runtime.streams.StreamNodeRegistration;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodecs;

public final class DefaultZLinkFrameworkOptions
    implements ZLinkFrameworkOptions,
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendHandlerOptions,
        systems.zlink.framework.runtime.internal.configuration
            .ZLinkLegacyTopologyOptions {
    private final ZLinkFrameworkRegistration registration = new ZLinkFrameworkRegistration();
    private final NetworkOptions network = new NetworkOptions();
    private final Map<String, ChannelKind> channelKinds = new HashMap<>();
    private final Map<String, ChannelRegistration> clientServerChannels =
        new HashMap<>();
    private final Set<String> spotMeshNames = new HashSet<>();
    private final Set<String> routeMeshNames = new HashSet<>();
    private final Set<Class<?>> spotFactoryTypes = new HashSet<>();
    private final Set<String> streamNodeNames = new HashSet<>();
    @Override
    public Duration defaultRequestTimeout() {
        return registration.defaultRequestTimeout();
    }

    @Override
    public void setDefaultRequestTimeout(Duration timeout) {
        registration.setDefaultRequestTimeout(requirePositive(timeout, "timeout"));
    }

    @Override
    public void setApplicationVersion(long version) {
        if (version < 0) {
            throw new ZLinkConfigurationException(
                "application version must not be negative");
        }
        registration.setApplicationVersion(version);
    }

    @Override
    public void setMaintenanceWave(String waveId) {
        if (waveId == null) {
            registration.setMaintenanceWave(null);
            return;
        }
        int encodedSize = waveId.getBytes(
            java.nio.charset.StandardCharsets.UTF_8).length;
        if (encodedSize < 1 || encodedSize > 255
            || waveId.indexOf('\0') >= 0) {
            throw new ZLinkConfigurationException(
                "maintenance wave must be 1..255 UTF-8 bytes without NUL");
        }
        registration.setMaintenanceWave(waveId);
    }

    @Override
    public ZLinkCodecRegistryBuilder codecs() {
        return registration.codecs();
    }

    @Override
    public void addHandlersFromPackageOf(Class<?> markerType) {
        registration.handlerPackageMarkers().add(Objects.requireNonNull(markerType, "markerType"));
    }

    @Override
    public ZLinkMetadataPolicyBuilder configureMetadata() {
        return registration.metadataPolicy();
    }

    @Override
    public ZLinkMeshNodeBuilder addRouteMesh(String meshName) {
        String name = requireName(meshName, "route mesh");
        addUnique(routeMeshNames, name, "route mesh");
        MeshNodeRegistration meshNode = new MeshNodeRegistration(
            name, network.bindHost(), network.advertiseHost().orElse(null));
        registration.meshNodes().add(meshNode);
        return meshNode;
    }

    public ClientServerChannelBuilder addClientServerChannel(String channelName)
    {
        String name = requireName(channelName, "channel");
        ChannelKind existing = channelKinds.get(name);
        if (existing != null && existing != ChannelKind.CLIENT_SERVER) {
            throw new ZLinkConfigurationException(
                "duplicate channel name: " + name);
        }
        ChannelRegistration channel = clientServerChannels.get(name);
        if (channel == null) {
            channel = new ChannelRegistration(name, ChannelKind.CLIENT_SERVER);
            clientServerChannels.put(name, channel);
            channelKinds.put(name, ChannelKind.CLIENT_SERVER);
            registration.channels().add(channel);
        }
        return ChannelBuilders.clientServer(
            channel, network.bindHost(), network.advertiseHost().orElse(null));
    }

    @Override
    public FanoutChannelBuilder addFanoutChannel(String channelName)
    {
        addChannel(channelName, ChannelKind.FANOUT);
        ChannelRegistration channel = new ChannelRegistration(channelName, ChannelKind.FANOUT);
        registration.channels().add(channel);
        return ChannelBuilders.fanout(
            channel, network.bindHost(), network.advertiseHost().orElse(null));
    }

    @Override
    public RouteMeshChannelBuilder addLegacyRouteMeshChannel(String channelName)
    {
        addChannel(channelName, ChannelKind.ROUTE_MESH);
        ChannelRegistration channel = new ChannelRegistration(channelName, ChannelKind.ROUTE_MESH);
        registration.channels().add(channel);
        return ChannelBuilders.routeMesh(channel);
    }

    @Override
    public SpotBuilders.Mesh addLegacySpotMesh(String channelName)
    {
        String meshName = requireName(channelName, "spot mesh");
        addUnique(spotMeshNames, meshName, "spot mesh");
        SpotNodeRegistration node = new SpotNodeRegistration(meshName, meshName);
        registration.spotNodes().add(node);
        return SpotBuilders.mesh(meshName, node, registration, this::addSpotFactoryType);
    }

    @Override
    public ZLinkStreamNodeBuilder addStreamNode(String streamNodeName)
    {
        String name = requireName(streamNodeName, "stream node");
        addUnique(streamNodeNames, name, "stream node");
        StreamNodeRegistration streamNode = new StreamNodeRegistration(name);
        registration.streamNodes().add(streamNode);
        return StreamBuilders.streamNode(
            streamNode, network.bindHost(), network.advertiseHost().orElse(null));
    }

    @Override
    public void addLocationStore(ZLinkLocationStore store) {
        registration.setLocationStore(Objects.requireNonNull(store, "store"));
    }

    @Override
    public void addRelocationStore(ZLinkRelocationStore store) {
        registration.setRelocationStore(
            Objects.requireNonNull(store, "store"));
    }

    @Override
    public ZLinkLocationOptions configureLocations() {
        return registration.locations().options();
    }

    @Override
    public systems.zlink.framework.configuration.ZLinkInboundDispatchOptions
        configureInboundDispatch() {
        return registration.inboundDispatch();
    }

    @Override
    public ZLinkNetworkOptions configureNetwork() {
        return network;
    }

    private static final class NetworkOptions implements ZLinkNetworkOptions {
        private String bindHost = "127.0.0.1";
        private String advertiseHost;

        @Override public String bindHost() { return bindHost; }

        @Override
        public void setBindHost(String host) {
            bindHost = requireHost(host, "bind host");
        }

        @Override
        public java.util.Optional<String> advertiseHost() {
            return java.util.Optional.ofNullable(advertiseHost);
        }

        @Override
        public void setAdvertiseHost(String host) {
            advertiseHost = requireHost(host, "advertise host");
        }

        private static String requireHost(String host, String label) {
            if (host == null || host.isBlank()) {
                throw new ZLinkConfigurationException(label + " must not be empty");
            }
            return host;
        }
    }

    @Override
    public void useFilter(Class<? extends ZLinkHandlerFilter> filterType) {
        Class<? extends ZLinkHandlerFilter> type =
            Objects.requireNonNull(filterType, "filterType");
        if (!registration.filters().contains(type)) {
            registration.filters().add(type);
        }
    }

    @Override
    public ZLinkDispatchOptions configureDispatch() {
        return registration.dispatchOptions();
    }

    @Override
    public ZLinkStreamCompressionBuilder configureStreamCompression() {
        return new DefaultStreamCompressionBuilder(registration);
    }

    @Override
    public ZLinkWorkerOptions configureWorkers() {
        return registration.workers();
    }

    @Override
    public void useVirtualThreadHandlers() {
        registration.useVirtualThreadHandlers();
    }

    @Override
    public void useHandlerExecutor(Executor executor) {
        registration.useHandlerExecutor(executor);
    }

    @Override
    public void useSuspendHandlerInvoker(ZLinkSuspendInvocationAdapter invoker) {
        registration.useSuspendHandlerInvoker(invoker);
    }

    private void addChannel(String channelName, ChannelKind kind) {
        String name = requireName(channelName, "channel");
        if (channelKinds.putIfAbsent(name, kind) != null) {
            throw new ZLinkConfigurationException(
                "duplicate channel name: " + name);
        }
    }

    private void addSpotFactoryType(Class<?> spotFactory) {
        if (!spotFactoryTypes.add(spotFactory)) {
            throw new ZLinkConfigurationException(
                "duplicate spot factory type: " + spotFactory.getName());
        }
    }

    public void validate() {
        registration.validate();
    }

    public ZLinkFrameworkRegistration registration() {
        return registration;
    }

    private static void addUnique(Set<String> names, String value, String label) {
        String name = requireName(value, label);
        if (!names.add(name)) {
            throw new ZLinkConfigurationException("duplicate " + label + " name: " + name);
        }
    }

    private static String requireName(String value, String label) {
        if (value == null || value.isBlank()) {
            throw new ZLinkConfigurationException(label + " name is required");
        }
        return value;
    }

    private static Duration requirePositive(Duration value, String label) {
        Objects.requireNonNull(value, label);
        if (value.isNegative() || value.isZero()) {
            throw new ZLinkConfigurationException(label + " must be positive");
        }
        return value;
    }

    private record DefaultStreamCompressionBuilder(ZLinkFrameworkRegistration registration)
        implements ZLinkStreamCompressionBuilder {
        @Override
        public ZLinkStreamCompressionBuilder useDefault() {
            registration.useStreamCompression(ZLinkStreamCompressionCodecs.lz4());
            return this;
        }

        @Override
        public ZLinkStreamCompressionBuilder useLz4() {
            return useDefault();
        }

        @Override
        public ZLinkStreamCompressionBuilder use(ZLinkStreamCompressionCodec codec) {
            registration.useStreamCompression(Objects.requireNonNull(codec, "codec"));
            return this;
        }

        @Override
        public ZLinkStreamCompressionBuilder disable() {
            registration.useStreamCompression(null);
            return this;
        }
    }
}
