package systems.zlink.framework.runtime.mesh;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.function.Consumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.configuration.ZLinkMeshChannelBuilder;
import systems.zlink.framework.configuration.ZLinkMeshChannelClientBuilder;
import systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder;
import systems.zlink.framework.configuration.ZLinkMeshNodeSocketConfig;
import systems.zlink.framework.configuration.ZLinkActorFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkMeshPeerConnection;
import systems.zlink.framework.configuration.ZLinkMeshPeerConnections;
import systems.zlink.framework.configuration.ZLinkSpotPublisherConfig;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.InstanceSpotFactoryConfiguration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableActorFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableInstanceSpotFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableSpotFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.UserSpotFactoryConfiguration;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;

public final class MeshNodeRegistration implements ZLinkMeshNodeBuilder {
    private final String meshName;
    private final Map<String, Channel> channels = new LinkedHashMap<>();
    private final List<Peer> peers = new ArrayList<>();
    private final List<DispatchHandler> routeHandlers = new ArrayList<>();
    private final List<Class<? extends ZLinkSpot<?>>> spotFactories = new ArrayList<>();
    private final List<Class<? extends ZLinkEntrySpot<?>>> entrySpots = new ArrayList<>();
    private final Map<String, Class<? extends ZLinkActorFactory>> actorFactories =
        new LinkedHashMap<>();
    private final Map<String, RelocatableSpotFactory<?>> relocatableSpotFactories =
        new LinkedHashMap<>();
    private final Map<String, RelocatableInstanceSpotFactory<?>> relocatableInstanceSpotFactories =
        new LinkedHashMap<>();
    private final Map<String, RelocatableActorFactory<?>> relocatableActorFactories =
        new LinkedHashMap<>();
    private final ObjectRoles objectRoles = new ObjectRoles();
    private final RouterSocketConfig routerSocket = new RouterSocketConfig();
    private final SpotPublisherConfig spotPublisher = new SpotPublisherConfig();
    private String bindEndpoint;
    private String bindHost;
    private String advertiseHost;
    private Integer listenPort;
    private RoutingId routingId;
    private String routingIdPrefix;
    private String entrySpotId;
    private int placementWeight = 100;
    private int actorCapacity;
    private int spotCapacity;
    private int activationConcurrency = 128;
    private Duration instanceSpotIdleTimeout = Duration.ZERO;
    private Duration defaultRequestTimeout;

    public MeshNodeRegistration(String meshName) {
        this(meshName, "127.0.0.1", null);
    }

    public MeshNodeRegistration(
        String meshName,
        String bindHost,
        String advertiseHost) {
        this.meshName = requireText(meshName, "mesh name");
        this.bindHost = requireText(bindHost, "bind host");
        this.advertiseHost = advertiseHost;
    }

    public String meshName() {
        return meshName;
    }

    public String bindEndpoint() {
        return bindEndpoint;
    }

    public synchronized RoutingId routingId() {
        if (routingId == null) {
            routingId = RoutingId.from(
                routingIdPrefix() + "-" + java.util.UUID.randomUUID());
        }
        return routingId;
    }

    public int placementWeight() {
        return placementWeight;
    }

    public int actorCapacity() {
        return actorCapacity;
    }

    public int spotCapacity() {
        return spotCapacity;
    }

    public int activationConcurrency() {
        return activationConcurrency;
    }

    public Duration instanceSpotIdleTimeout() {
        return instanceSpotIdleTimeout;
    }

    public String routingIdPrefix() {
        return routingIdPrefix == null ? meshName : routingIdPrefix;
    }

    public String entrySpotId() {
        return ensureEntrySpotId();
    }

    public List<String> channelNames() {
        return channels.entrySet().stream()
            .filter(entry -> entry.getValue().client || entry.getValue().server)
            .map(Map.Entry::getKey)
            .toList();
    }

    public Map<String, Integer> channelWeights() {
        Map<String, Integer> weights = new LinkedHashMap<>();
        channels.forEach((name, channel) -> {
            if (channel.server) {
                weights.put(name, channel.weight);
            }
        });
        return Map.copyOf(weights);
    }

    public List<Peer> peers() {
        return List.copyOf(peers);
    }

    public List<DispatchHandler> nodeHandlers() {
        return List.copyOf(routeHandlers);
    }

    public Map<String, List<DispatchHandler>> channelHandlers() {
        Map<String, List<DispatchHandler>> result = new LinkedHashMap<>();
        channels.forEach((name, channel) ->
            result.put(name, List.copyOf(channel.handlers)));
        return Map.copyOf(result);
    }

    public Map<String, List<String>> channelHandlerGroups() {
        Map<String, List<String>> result = new LinkedHashMap<>();
        channels.forEach((name, channel) ->
            result.put(name, List.copyOf(channel.handlerGroups)));
        return Map.copyOf(result);
    }

    public List<Class<? extends ZLinkSpot<?>>> spotFactories() {
        return List.copyOf(spotFactories);
    }

    public List<Class<? extends ZLinkEntrySpot<?>>> entrySpots() {
        return List.copyOf(entrySpots);
    }

    public Map<String, Class<? extends ZLinkActorFactory>> actorFactories() {
        return Map.copyOf(actorFactories);
    }

    public Map<String, RelocatableSpotFactory<?>> relocatableSpotFactories() {
        return Map.copyOf(relocatableSpotFactories);
    }

    public Map<String, RelocatableInstanceSpotFactory<?>> relocatableInstanceSpotFactories() {
        return Map.copyOf(relocatableInstanceSpotFactories);
    }

    public Map<String, RelocatableActorFactory<?>> relocatableActorFactories() {
        return Map.copyOf(relocatableActorFactories);
    }

    public boolean objectRoleEnabled() {
        return objectRoles.client || objectRoles.server;
    }

    public boolean objectServer() {
        return objectRoles.server;
    }

    public boolean requiresRelocationStore() {
        return java.util.stream.Stream.of(
                relocatableSpotFactories.values().stream()
                    .map(RelocatableSpotFactory::relocationPolicy),
                relocatableInstanceSpotFactories.values().stream()
                    .map(RelocatableInstanceSpotFactory::relocationPolicy),
                relocatableActorFactories.values().stream()
                    .map(RelocatableActorFactory::relocationPolicy))
            .flatMap(java.util.function.Function.identity())
            .anyMatch(policy -> !(policy instanceof RelocationPolicy.Disabled));
    }

    public Duration defaultRequestTimeout() {
        return defaultRequestTimeout;
    }

    public Set<Class<?>> applicationTypes() {
        Set<Class<?>> types = new LinkedHashSet<>();
        routeHandlers.forEach(handler -> types.add(handler.handlerType()));
        channels.values().forEach(channel ->
            channel.handlers.forEach(handler -> types.add(handler.handlerType())));
        types.addAll(spotFactories);
        types.addAll(entrySpots);
        types.addAll(actorFactories.values());
        relocatableSpotFactories.values().forEach(factory -> {
            types.add(factory.spotType());
            addRelocationAdapterType(types, factory.relocationPolicy());
        });
        relocatableInstanceSpotFactories.values().forEach(factory -> {
            types.add(factory.spotType());
            addRelocationAdapterType(types, factory.relocationPolicy());
        });
        relocatableActorFactories.values().forEach(factory -> {
            types.add(factory.actorType());
            types.add(factory.factoryType());
            addRelocationAdapterType(types, factory.relocationPolicy());
        });
        return Set.copyOf(types);
    }

    private static void addRelocationAdapterType(
        Set<Class<?>> types,
        RelocationPolicy policy) {
        if (policy instanceof RelocationPolicy.PreserveState preserveState) {
            types.add(preserveState.adapterClass());
        }
    }

    @Override
    public ZLinkMeshChannelBuilder channelName(String channelName) {
        String name = requireText(channelName, "channel name");
        Channel channel = new Channel(name);
        if (channels.putIfAbsent(name, channel) != null) {
            throw new ZLinkConfigurationException(
                "duplicate channel name on RouteMesh " + meshName + ": " + name);
        }
        return channel;
    }

    @Override
    public ZLinkMeshNodeBuilder listen(String endpoint) {
        bindEndpoint = requireText(endpoint, "MeshNode listen endpoint");
        listenPort = null;
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder listen() {
        return listen(0);
    }

    @Override
    public ZLinkMeshNodeBuilder listen(int port) {
        if (port < 0 || port > 65_535) {
            throw new ZLinkConfigurationException(
                "MeshNode listen port must be between 0 and 65535");
        }
        listenPort = port;
        updateDefaultEndpoint();
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setBindHost(String host) {
        bindHost = requireText(host, "bind host");
        updateDefaultEndpoint();
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setAdvertiseHost(String host) {
        advertiseHost = requireText(host, "advertise host");
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setRoutingId(RoutingId value) {
        routingId = Objects.requireNonNull(value, "routingId");
        return this;
    }

    public String advertisedEndpoint(String actualEndpoint) {
        if (advertiseHost == null || actualEndpoint == null
            || !actualEndpoint.startsWith("tcp://")) {
            return actualEndpoint;
        }
        int colon = actualEndpoint.lastIndexOf(':');
        return colon < "tcp://".length()
            ? actualEndpoint
            : "tcp://" + advertiseHost + actualEndpoint.substring(colon);
    }

    private void updateDefaultEndpoint() {
        if (listenPort != null) {
            bindEndpoint = "tcp://" + bindHost + ":" + listenPort;
        }
    }

    @Override
    public ZLinkMeshNodeBuilder setRoutingIdPrefix(String value) {
        routingIdPrefix = requireText(value, "routing ID prefix");
        routingId = null;
        entrySpotId = null;
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setPlacementWeight(int value) {
        if (value < 0 || value > 10_000) {
            throw new ZLinkConfigurationException(
                "placement weight must be in range 0..10000");
        }
        placementWeight = value;
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setActorCapacity(int maxActors) {
        if (maxActors < 0) {
            throw new ZLinkConfigurationException(
                "Actor capacity must not be negative.");
        }
        actorCapacity = maxActors;
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setSpotCapacity(int maxSpots) {
        if (maxSpots < 0) {
            throw new ZLinkConfigurationException(
                "Spot capacity must not be negative.");
        }
        spotCapacity = maxSpots;
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setActivationConcurrency(
        int maxConcurrentActivations) {
        if (maxConcurrentActivations <= 0) {
            throw new ZLinkConfigurationException(
                "Activation concurrency must be positive.");
        }
        activationConcurrency = maxConcurrentActivations;
        return this;
    }

    @Override
    public ZLinkMeshNodeBuilder setInstanceSpotIdleTimeout(Duration timeout) {
        if (timeout == null || timeout.isNegative()) {
            throw new ZLinkConfigurationException(
                "Instance Spot idle timeout must be zero or positive.");
        }
        instanceSpotIdleTimeout = timeout;
        return this;
    }

    @Override
    public ZLinkMeshNodeSocketConfig configureRouterSocket() {
        return routerSocket;
    }

    @Override
    public ZLinkSpotPublisherConfig configureSpotPublisher() {
        return spotPublisher;
    }

    @Override
    public ZLinkMeshPeerConnections peerConnections() {
        return new PeerConnections();
    }

    @Override
    public ZLinkMeshNodeBuilder setDefaultRequestTimeout(Duration timeout) {
        defaultRequestTimeout = requirePositive(timeout, "default request timeout");
        return this;
    }

    @Override
    public ZLinkMeshObjectRoleBuilder objects() {
        return objectRoles;
    }

    @Override
    public <THandler, TMessage>
    ZLinkMeshNodeBuilder addRouteSendHandler(
        Class<THandler> handlerType,
        Class<TMessage> messageType) {
        routeHandlers.add(new DispatchHandler(
            Objects.requireNonNull(handlerType, "handlerType"),
            Objects.requireNonNull(messageType, "messageType"),
            null));
        return this;
    }

    @Override
    public <THandler, TRequest, TReply>
    ZLinkMeshNodeBuilder addRouteRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType) {
        routeHandlers.add(new DispatchHandler(
            Objects.requireNonNull(handlerType, "handlerType"),
            Objects.requireNonNull(requestType, "requestType"),
            Objects.requireNonNull(replyType, "replyType")));
        return this;
    }

    private void registerEntrySpot(
        Class<? extends ZLinkEntrySpot<?>> entrySpotType) {
        entrySpots.add(Objects.requireNonNull(entrySpotType, "entrySpotType"));
    }

    public void validate() {
        if (bindEndpoint == null) {
            throw new ZLinkConfigurationException(
                "MeshNode listen endpoint is required: " + meshName);
        }
        routingId();
        if (entrySpots.size() > 1) {
            throw new ZLinkConfigurationException(
                "MeshNode registers multiple entry spots: " + meshName);
        }
        channels.forEach((name, channel) -> {
            if (channel.client == channel.server) {
                throw new ZLinkConfigurationException(
                    "RouteMesh channel requires exactly one role: " + name);
            }
        });
        Set<Class<? extends ZLinkSpot<?>>> spotTypes = new LinkedHashSet<>();
        for (Class<? extends ZLinkSpot<?>> spotFactory : spotFactories) {
            if (!spotTypes.add(spotFactory)) {
                throw new ZLinkConfigurationException(
                    "duplicate spot factory type on MeshNode: " + meshName);
            }
        }
        relocatableSpotFactories.values().forEach(factory ->
            validateRelocationPolicy(
                factory.spotType(),
                factory.relocationPolicy(),
                ZLinkSpotRelocationAdapter.class,
                "Spot"));
        relocatableInstanceSpotFactories.values().forEach(factory ->
            validateRelocationPolicy(
                factory.spotType(),
                factory.relocationPolicy(),
                ZLinkSpotRelocationAdapter.class,
                "Instance Spot"));
        relocatableActorFactories.values().forEach(factory ->
            validateRelocationPolicy(
                factory.actorType(),
                factory.relocationPolicy(),
                ZLinkActorRelocationAdapter.class,
                "Actor"));
    }

    /**
     * Validates the listener limit required by a host-wide application HWM.
     * This method is called during registration validation; it is not a
     * runtime socket mutation API.
     */
    public void validateApplicationHwm(long applicationHwmBytes) {
        if (applicationHwmBytes == 0) {
            return;
        }
        if (routerSocket.maxMessageSize() <= 0) {
            throw new ZLinkConfigurationException(
                "Application HWM requires a finite positive MaxMessageSize on MeshNode: "
                    + meshName);
        }
    }

    private static void validateRelocationPolicy(
        Class<?> objectType,
        RelocationPolicy policy,
        Class<?> adapterContract,
        String label) {
        Objects.requireNonNull(policy, "relocationPolicy");
        if (!(policy instanceof RelocationPolicy.PreserveState preserveState)) {
            return;
        }
        Class<?> adapterType = Objects.requireNonNull(
            preserveState.adapterClass(),
            "relocation adapterClass");
        if (!adapterContract.isAssignableFrom(adapterType)) {
            throw new ZLinkConfigurationException(
                label + " relocation adapter must implement "
                    + adapterContract.getSimpleName() + ": "
                    + adapterType.getName());
        }
        if (!ZLinkRelocationAdapterTypeMatcher.matches(
            adapterType,
            adapterContract,
            objectType)) {
            throw new ZLinkConfigurationException(
                label + " relocation adapter type does not match "
                    + objectType.getName() + ": " + adapterType.getName());
        }
    }

    private final class ObjectRoles
        implements ZLinkMeshObjectRoleBuilder,
            ZLinkMeshObjectClientBuilder,
            ZLinkMeshObjectServerBuilder {
        private boolean client;
        private boolean server;

        @Override
        public ZLinkMeshObjectClientBuilder client() {
            client = true;
            return this;
        }

        @Override
        public ZLinkMeshObjectServerBuilder server() {
            client = true;
            server = true;
            ensureEntrySpotId();
            return this;
        }

        @Override
        public ZLinkMeshObjectServerBuilder addEntrySpot(
            Class<? extends ZLinkEntrySpot<?>> entrySpotType) {
            MeshNodeRegistration.this.registerEntrySpot(entrySpotType);
            return this;
        }

        @Override
        public <TSpot extends ZLinkSpot<?>>
        ZLinkMeshObjectServerBuilder addSpotFactory(
            String stableType,
            Class<TSpot> spotType,
            Consumer<ZLinkUserSpotFactoryBuilder<TSpot>> configure) {
            String type = requireStableType(stableType);
            UserSpotFactoryBuilder<TSpot> builder = new UserSpotFactoryBuilder<>();
            builder.configure(Objects.requireNonNull(configure, "configure"));
            RelocationPolicy relocationPolicy =
                builder.requireRelocationPolicy();
            RelocatableSpotFactory<TSpot> value = new RelocatableSpotFactory<>(
                type,
                Objects.requireNonNull(spotType, "spotType"),
                builder.build(relocationPolicy),
                relocationPolicy);
            putUnique(relocatableSpotFactories, type, value, "Spot stable type");
            spotFactories.add(spotType);
            return this;
        }

        @Override
        public <TSpot extends ZLinkInstanceSpot>
        ZLinkMeshObjectServerBuilder addInstanceSpotFactory(
            String stableType,
            Class<TSpot> spotType,
            Consumer<ZLinkInstanceSpotFactoryBuilder<TSpot>> configure) {
            String type = requireStableType(stableType);
            InstanceSpotFactoryBuilder<TSpot> builder =
                new InstanceSpotFactoryBuilder<>();
            builder.configure(Objects.requireNonNull(configure, "configure"));
            RelocatableInstanceSpotFactory<TSpot> value =
                new RelocatableInstanceSpotFactory<>(
                    type,
                    Objects.requireNonNull(spotType, "spotType"),
                    builder.build(),
                    builder.requireRelocationPolicy());
            putUnique(
                relocatableInstanceSpotFactories,
                type,
                value,
                "Instance Spot stable type");
            return this;
        }

        @Override
        public <TActor extends ZLinkActor>
        ZLinkMeshObjectServerBuilder addActorFactory(
            String stableType,
            Class<TActor> actorType,
            Class<? extends ZLinkActorFactory> factoryType,
            Consumer<ZLinkActorFactoryBuilder<TActor>> configure) {
            String type = requireStableType(stableType);
            ActorFactoryBuilder<TActor> builder = new ActorFactoryBuilder<>();
            builder.configure(Objects.requireNonNull(configure, "configure"));
            RelocatableActorFactory<TActor> value = new RelocatableActorFactory<>(
                type,
                Objects.requireNonNull(actorType, "actorType"),
                Objects.requireNonNull(factoryType, "factoryType"),
                builder.requireRelocationPolicy());
            putUnique(relocatableActorFactories, type, value, "Actor stable type");
            putUnique(actorFactories, type, factoryType, "actor factory");
            return this;
        }
    }

    private String ensureEntrySpotId() {
        if (entrySpotId == null) {
            entrySpotId = routingIdPrefix()
                + "-entry-"
                + java.util.UUID.randomUUID().toString()
                    .toLowerCase(java.util.Locale.ROOT);
        }
        return entrySpotId;
    }

    private static String requireStableType(String value) {
        return requireText(value, "stable type");
    }

    private static String requireText(String value, String label) {
        if (value == null || value.isBlank()) {
            throw new ZLinkConfigurationException(label + " is required");
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

    private static <T> void putUnique(
        Map<String, T> values,
        String key,
        T value,
        String label) {
        String name = requireText(key, "actor type");
        if (values.putIfAbsent(name, Objects.requireNonNull(value, "value")) != null) {
            throw new ZLinkConfigurationException("duplicate " + label + ": " + name);
        }
    }

    public record Peer(String endpoint, RoutingId expectedRoutingId) {
    }

    public record DispatchHandler(
        Class<?> handlerType,
        Class<?> messageType,
        Class<?> replyType) {
        public boolean request() {
            return replyType != null;
        }
    }

    private abstract static class RelocationSelection {
        private RelocationPolicy relocationPolicy;
        private boolean accepting = true;

        final <TBuilder> void configure(Consumer<TBuilder> configure) {
            try {
                @SuppressWarnings("unchecked")
                TBuilder builder = (TBuilder) this;
                configure.accept(builder);
            } finally {
                accepting = false;
            }
        }

        final void select(RelocationPolicy value) {
            requireAccepting();
            if (relocationPolicy != null) {
                throw new ZLinkConfigurationException(
                    "factory relocation behavior must be selected exactly once");
            }
            relocationPolicy = Objects.requireNonNull(value, "value");
        }

        final RelocationPolicy requireRelocationPolicy() {
            if (relocationPolicy == null) {
                throw new ZLinkConfigurationException(
                    "factory relocation behavior must be selected exactly once");
            }
            return relocationPolicy;
        }

        final void requireAccepting() {
            if (!accepting) {
                throw new ZLinkConfigurationException(
                    "factory builder is valid only during its configure callback");
            }
        }

        final Class<?> requireAdapterClass(Class<?> adapterClass) {
            if (adapterClass == null) {
                throw new ZLinkConfigurationException(
                    "relocation adapterClass is required");
            }
            return adapterClass;
        }
    }

    private static final class ActorFactoryBuilder<TActor extends ZLinkActor>
        extends RelocationSelection
        implements ZLinkActorFactoryBuilder<TActor> {
        @Override
        public void disableRelocation() {
            select(new RelocationPolicy.Disabled());
        }

        @Override
        public void recreateOnRelocation() {
            select(new RelocationPolicy.Recreate());
        }

        @Override
        public void preserveStateWith(
            Class<? extends ZLinkActorRelocationAdapter<TActor>> adapterClass) {
            select(new RelocationPolicy.PreserveState(
                requireAdapterClass(adapterClass)));
        }
    }

    private static final class UserSpotFactoryBuilder<TSpot extends ZLinkSpot<?>>
        extends RelocationSelection
        implements ZLinkUserSpotFactoryBuilder<TSpot> {
        private int stableTypeLimit;
        private ZLinkUserSpotExecutionMode executionMode =
            ZLinkUserSpotExecutionMode.SPOT_WIDE;
        private ZLinkSpotRelocationReadinessMode relocationReadiness =
            ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY;

        @Override
        public ZLinkUserSpotFactoryBuilder<TSpot> stableTypeLimit(int limit) {
            requireAccepting();
            stableTypeLimit = requirePositiveLimit(limit, "stableTypeLimit");
            return this;
        }

        @Override
        public ZLinkUserSpotFactoryBuilder<TSpot> executionMode(
            ZLinkUserSpotExecutionMode mode) {
            requireAccepting();
            executionMode = Objects.requireNonNull(mode, "mode");
            return this;
        }

        @Override
        public ZLinkUserSpotFactoryBuilder<TSpot> relocationReadiness(
            ZLinkSpotRelocationReadinessMode mode) {
            requireAccepting();
            relocationReadiness = Objects.requireNonNull(mode, "mode");
            return this;
        }

        @Override
        public void disableRelocation() {
            select(new RelocationPolicy.Disabled());
        }

        @Override
        public void recreateOnRelocation() {
            select(new RelocationPolicy.Recreate());
        }

        @Override
        public void preserveStateWith(
            Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass) {
            select(new RelocationPolicy.PreserveState(
                requireAdapterClass(adapterClass)));
        }

        UserSpotFactoryConfiguration build(RelocationPolicy policy) {
            if (executionMode != ZLinkUserSpotExecutionMode.SPOT_WIDE
                && relocationReadiness
                    != ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY) {
                throw new ZLinkConfigurationException(
                    "relocationReadiness applies only to SpotWide User Spots");
            }
            if (executionMode == ZLinkUserSpotExecutionMode.PER_ACTOR
                && !(policy instanceof RelocationPolicy.Recreate)) {
                throw new ZLinkConfigurationException(
                    "PerActor User Spots require RecreateOnRelocation");
            }
            return new UserSpotFactoryConfiguration(
                stableTypeLimit,
                executionMode,
                relocationReadiness);
        }
    }

    private static final class InstanceSpotFactoryBuilder<
        TSpot extends ZLinkInstanceSpot>
        extends RelocationSelection
        implements ZLinkInstanceSpotFactoryBuilder<TSpot> {
        private int stableTypeLimit;

        @Override
        public ZLinkInstanceSpotFactoryBuilder<TSpot> stableTypeLimit(int limit) {
            requireAccepting();
            stableTypeLimit = requirePositiveLimit(limit, "stableTypeLimit");
            return this;
        }

        @Override
        public void disableRelocation() {
            select(new RelocationPolicy.Disabled());
        }

        @Override
        public void recreateOnRelocation() {
            select(new RelocationPolicy.Recreate());
        }

        @Override
        public void preserveStateWith(
            Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass) {
            select(new RelocationPolicy.PreserveState(
                requireAdapterClass(adapterClass)));
        }

        InstanceSpotFactoryConfiguration build() {
            return new InstanceSpotFactoryConfiguration(stableTypeLimit);
        }
    }

    private static int requirePositiveLimit(int value, String label) {
        if (value <= 0) {
            throw new ZLinkConfigurationException(label + " must be positive");
        }
        return value;
    }

    private final class PeerConnections implements ZLinkMeshPeerConnections {
        @Override
        public void connect(String endpoint) {
            peers.add(new Peer(requireText(endpoint, "peer endpoint"), null));
        }

        @Override
        public void connect(RoutingId expectedRoutingId, String endpoint) {
            peers.add(new Peer(
                requireText(endpoint, "peer endpoint"),
                Objects.requireNonNull(expectedRoutingId, "expectedRoutingId")));
        }

        @Override
        public void disconnect(String endpoint) {
            String target = requireText(endpoint, "peer endpoint");
            peers.removeIf(peer -> peer.endpoint().equals(target));
        }

        @Override
        public List<ZLinkMeshPeerConnection> listConnections() {
            return peers.stream()
                .map(peer -> new ZLinkMeshPeerConnection(
                    peer.endpoint(),
                    Optional.ofNullable(peer.expectedRoutingId())))
                .toList();
        }
    }

    private final class Channel implements
        ZLinkMeshChannelBuilder,
        ZLinkMeshChannelClientBuilder,
        ZLinkMeshChannelServerBuilder {
        private final String name;
        private final List<String> handlerGroups = new ArrayList<>();
        private final List<DispatchHandler> handlers = new ArrayList<>();
        private int weight = 100;
        private boolean client;
        private boolean server;

        private Channel(String name) {
            this.name = name;
        }

        @Override
        public ZLinkMeshChannelClientBuilder client() {
            selectRole(false);
            return this;
        }

        @Override
        public ZLinkMeshChannelServerBuilder server() {
            selectRole(true);
            return this;
        }

        @Override
        public ZLinkMeshChannelServerBuilder setWeight(int value) {
            if (value < 0 || value > 10_000) {
                throw new ZLinkConfigurationException(
                    "channel weight must be in 0..10000");
            }
            weight = value;
            return this;
        }

        @Override
        public ZLinkMeshChannelServerBuilder addHandlerGroup(String groupName) {
            handlerGroups.add(requireText(groupName, "handler group"));
            return this;
        }

        @Override
        public <THandler extends ZLinkSendHandler<TMessage>, TMessage>
        ZLinkMeshChannelServerBuilder addSendHandler(
            Class<THandler> handlerType,
            Class<TMessage> messageType) {
            handlers.add(new DispatchHandler(
                Objects.requireNonNull(handlerType, "handlerType"),
                Objects.requireNonNull(messageType, "messageType"),
                null));
            return this;
        }

        @Override
        public <THandler extends ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply>
        ZLinkMeshChannelServerBuilder addRequestHandler(
            Class<THandler> handlerType,
            Class<TRequest> requestType,
            Class<TReply> replyType) {
            handlers.add(new DispatchHandler(
                Objects.requireNonNull(handlerType, "handlerType"),
                Objects.requireNonNull(requestType, "requestType"),
                Objects.requireNonNull(replyType, "replyType")));
            return this;
        }

        private void selectRole(boolean serverRole) {
            if (client || server) {
                throw new ZLinkConfigurationException(
                    "RouteMesh channel role is already selected: " + name);
            }
            client = !serverRole;
            server = serverRole;
        }
    }

    private static final class RouterSocketConfig implements ZLinkMeshNodeSocketConfig {
        private long maxMessageSize = 16_777_216L;
        //  HWM is an accounted byte count, so it has to be 64-bit. The mailbox
        //  budgets default to 0, which leaves the runtime default in place.
        private long sendHighWaterMark = 1000;
        private long receiveHighWaterMark = 1000;
        private long mailboxMessageBudget;
        private long mailboxByteBudget;
        private Duration receiveTimeout;
        private Duration sendTimeout;

        @Override public long maxMessageSize() { return maxMessageSize; }
        @Override
        public void setMaxMessageSize(long value) {
            if (value < 0) {
                throw new ZLinkConfigurationException(
                    "MaxMessageSize must be zero or a positive byte count.");
            }
            maxMessageSize = value;
        }
        @Override public long sendHighWaterMark() { return sendHighWaterMark; }
        @Override public void setSendHighWaterMark(long value) { sendHighWaterMark = value; }
        @Override public long receiveHighWaterMark() { return receiveHighWaterMark; }
        @Override public void setReceiveHighWaterMark(long value) { receiveHighWaterMark = value; }
        @Override public long mailboxMessageBudget() { return mailboxMessageBudget; }
        @Override public void setMailboxMessageBudget(long value) { mailboxMessageBudget = value; }
        @Override public long mailboxByteBudget() { return mailboxByteBudget; }
        @Override public void setMailboxByteBudget(long value) { mailboxByteBudget = value; }
        @Override public Optional<Duration> receiveTimeout() {
            return Optional.ofNullable(receiveTimeout);
        }
        @Override public void setReceiveTimeout(Duration value) { receiveTimeout = value; }
        @Override public Optional<Duration> sendTimeout() {
            return Optional.ofNullable(sendTimeout);
        }
        @Override public void setSendTimeout(Duration value) {
            sendTimeout = value == null ? null : requireSendTimeout(value);
        }
    }

    private static final class SpotPublisherConfig implements ZLinkSpotPublisherConfig {
        private int sendHighWaterMark = 1000;
        private Duration sendTimeout;
        private Duration linger;

        @Override public int sendHighWaterMark() { return sendHighWaterMark; }
        @Override public void setSendHighWaterMark(int value) { sendHighWaterMark = value; }
        @Override public Optional<Duration> sendTimeout() {
            return Optional.ofNullable(sendTimeout);
        }
        @Override public void setSendTimeout(Duration value) {
            sendTimeout = value == null ? null : requireSendTimeout(value);
        }
        @Override public Optional<Duration> linger() { return Optional.ofNullable(linger); }
        @Override public void setLinger(Duration value) { linger = value; }
    }

    private static Duration requireSendTimeout(Duration value) {
        if (value.isZero() || value.isNegative()) {
            throw new ZLinkConfigurationException("send timeout must be positive");
        }
        long seconds = value.getSeconds();
        if (seconds > Integer.MAX_VALUE / 1000L) {
            throw new ZLinkConfigurationException(
                "send timeout must normalize to at most Integer.MAX_VALUE ms");
        }
        long millis = seconds * 1000L
            + (value.getNano() + 999_999L) / 1_000_000L;
        if (millis > Integer.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                "send timeout must normalize to at most Integer.MAX_VALUE ms");
        }
        return value;
    }
}
