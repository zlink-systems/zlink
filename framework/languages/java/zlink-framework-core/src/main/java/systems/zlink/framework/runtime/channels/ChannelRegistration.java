package systems.zlink.framework.runtime.channels;

import java.time.Duration;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;

public final class ChannelRegistration {
    private final String name;
    private final ChannelKind kind;
    private final ClientServerState clientServer = new ClientServerState();
    private final FanoutState fanout = new FanoutState();
    private final RouteMeshState routeMesh = new RouteMeshState();
    private final ConfiguredSocketRuntimeOptions serverSocketOptions =
        new ConfiguredSocketRuntimeOptions();
    private final RuntimeEndpointConnections clientConnections;
    private final RuntimeEndpointConnections subscriberConnections;
    private final RuntimeEndpointConnections routeConnections;
    private final Set<String> handlerGroups = new LinkedHashSet<>();
    private RoutingId routingId;
    private String routingIdPrefix;
    private RoutingId routeRoutingId;
    private Duration defaultRequestTimeout;

    public ChannelRegistration(String name, ChannelKind kind) {
        this.name = name;
        this.kind = kind;
        clientConnections = new RuntimeEndpointConnections(
            this::enableClientFromConnection,
            clientServer.manualEndpoints);
        subscriberConnections = new RuntimeEndpointConnections(this::enableSubscriber, fanout.subscriberManualEndpoints);
        routeConnections = new RuntimeEndpointConnections(this::enableClient, routeMesh.manualEndpoints);
    }

    public String name() {
        return name;
    }

    public ChannelKind kind() {
        return kind;
    }

    ZLinkSocketRuntimeOptions serverSocketOptions() {
        return serverSocketOptions;
    }

    List<String> serverBinds() {
        return clientServer.serverBinds;
    }

    String clientServerAdvertiseHost() {
        return clientServer.advertiseHost;
    }

    List<String> clientManualEndpoints() {
        return clientConnections.listConnections();
    }

    List<ChannelRequestHandlerRegistration> requestHandlers() {
        return clientServer.requestHandlers;
    }

    List<ChannelSendHandlerRegistration> sendHandlers() {
        return clientServer.sendHandlers;
    }

    List<String> publisherBinds() {
        return fanout.publisherBinds;
    }

    String fanoutAdvertiseHost() {
        return fanout.advertiseHost;
    }

    List<String> subscriberManualEndpoints() {
        return subscriberConnections.listConnections();
    }

    List<ChannelPublishHandlerRegistration> publishHandlers() {
        return fanout.publishHandlers;
    }

    List<String> routeBinds() {
        return routeMesh.binds;
    }

    List<String> routeManualEndpoints() {
        return routeConnections.listConnections();
    }

    RuntimeEndpointConnections clientConnections() { return clientConnections; }

    RuntimeEndpointConnections subscriberConnections() { return subscriberConnections; }

    RuntimeEndpointConnections routeConnections() { return routeConnections; }

    void detachRuntimeConnections() {
        clientConnections.detach();
        subscriberConnections.detach();
        routeConnections.detach();
    }

    List<ChannelRouteRequestHandlerRegistration> routeRequestHandlers() {
        return routeMesh.requestHandlers;
    }

    List<ChannelRouteSendHandlerRegistration> routeSendHandlers() {
        return routeMesh.sendHandlers;
    }

    public List<String> handlerGroups() {
        return List.copyOf(handlerGroups);
    }

    public List<Class<?>> handlerTypes() {
        List<Class<?>> types = new ArrayList<>();
        for (ChannelSendHandlerRegistration handler : clientServer.sendHandlers) {
            types.add(handler.handlerType());
        }
        for (ChannelRequestHandlerRegistration handler : clientServer.requestHandlers) {
            types.add(handler.handlerType());
        }
        for (ChannelPublishHandlerRegistration handler : fanout.publishHandlers) {
            types.add(handler.handlerType());
        }
        for (ChannelRouteSendHandlerRegistration handler : routeMesh.sendHandlers) {
            types.add(handler.handlerType());
        }
        for (ChannelRouteRequestHandlerRegistration handler : routeMesh.requestHandlers) {
            types.add(handler.handlerType());
        }
        return List.copyOf(types);
    }

    public RoutingId routeRoutingId() {
        return routeRoutingId;
    }

    public RoutingId routingId() {
        return routingId;
    }

    String routingIdPrefix() {
        return routingIdPrefix == null ? name : routingIdPrefix;
    }

    public Duration defaultRequestTimeout() {
        return defaultRequestTimeout;
    }

    void setDefaultRequestTimeout(Duration timeout) {
        if (timeout == null || timeout.isNegative() || timeout.isZero()) {
            throw new ZLinkConfigurationException("request timeout must be greater than zero");
        }
        defaultRequestTimeout = timeout;
    }

    boolean clientEnabled() {
        return clientServer.clientEnabled || routeMesh.clientEnabled;
    }

    boolean clientServerClientEnabled() {
        return clientServer.clientEnabled;
    }

    boolean clientServerServerEnabled() {
        return clientServer.serverEnabled;
    }

    boolean publisherEnabled() {
        return fanout.publisherEnabled;
    }

    boolean subscriberEnabled() {
        return fanout.subscriberEnabled;
    }

    boolean automaticSubscriberEnabled() {
        return fanout.automaticSubscriberEnabled;
    }

    /**
     * Reports registration-only topology that cannot prove continuity during
     * automatic host retirement. Connection state is deliberately ignored.
     */
    public boolean blocksAutomaticRetire(boolean locationStoreAvailable) {
        return switch (kind) {
            case ROUTE_MESH -> !routeConnections.listConnections().isEmpty();
            case CLIENT_SERVER ->
                !clientConnections.listConnections().isEmpty();
            case FANOUT ->
                !subscriberConnections.listConnections().isEmpty()
                    || (fanout.publisherEnabled && !locationStoreAvailable);
        };
    }

    void enableClient() {
        if (kind == ChannelKind.ROUTE_MESH) {
            routeMesh.clientEnabled = true;
        } else {
            clientServer.clientEnabled = true;
        }
    }

    void declareClient() {
        if (clientServer.clientDeclared) {
            clientServer.duplicateClientDeclaration = true;
        }
        clientServer.clientDeclared = true;
        enableClient();
    }

    private void enableClientFromConnection() {
        clientServer.clientDeclared = true;
        enableClient();
    }

    void enableServer() {
        clientServer.serverEnabled = true;
    }

    void declareServer() {
        if (clientServer.serverDeclared) {
            clientServer.duplicateServerDeclaration = true;
        }
        clientServer.serverDeclared = true;
        enableServer();
    }

    void enablePublisher() {
        fanout.publisherEnabled = true;
    }

    void enableSubscriber() {
        fanout.subscriberEnabled = true;
    }

    void enableAutomaticSubscriber() {
        fanout.subscriberEnabled = true;
        fanout.automaticSubscriberEnabled = true;
    }

    void addServerBind(String endpoint) {
        clientServer.serverBinds.add(requireEndpoint(endpoint));
    }

    void replaceClientServerBind(String endpoint) {
        clientServer.serverBinds.clear();
        clientServer.serverBinds.add(requireEndpoint(endpoint));
    }

    void setClientServerAdvertiseHost(String host) {
        clientServer.advertiseHost = host;
    }

    void setRoutingId(RoutingId routingId) {
        if (routingId == null) {
            throw new ZLinkConfigurationException(
                kind == ChannelKind.FANOUT
                    ? "fanout channel routing id is required: " + name
                    : "client/server channel routing id is required: " + name);
        }
        this.routingId = routingId;
    }

    void setRoutingIdPrefix(String prefix) {
        if (prefix == null || prefix.isBlank()) {
            throw new ZLinkConfigurationException("routing ID prefix is required: " + name);
        }
        routingIdPrefix = prefix;
        routingId = null;
    }

    void addClientManualEndpoint(String endpoint) {
        clientConnections.connect(endpoint);
    }

    void removeClientManualEndpoint(String endpoint) {
        clientConnections.disconnect(endpoint);
    }

    void addPublisherBind(String endpoint) {
        fanout.publisherBinds.add(requireEndpoint(endpoint));
    }

    void replacePublisherBind(String endpoint) {
        fanout.publisherBinds.clear();
        fanout.publisherBinds.add(requireEndpoint(endpoint));
    }

    void setFanoutAdvertiseHost(String host) {
        fanout.advertiseHost = host;
    }

    void addSubscriberManualEndpoint(String endpoint) {
        subscriberConnections.connect(endpoint);
    }

    void removeSubscriberManualEndpoint(String endpoint) {
        subscriberConnections.disconnect(endpoint);
    }

    void setRouteRoutingId(RoutingId routingId) {
        routeRoutingId = routingId;
    }

    void addRouteBind(String endpoint) {
        routeMesh.binds.add(requireEndpoint(endpoint));
    }

    void addRouteManualEndpoint(String endpoint) {
        routeConnections.connect(endpoint);
    }

    void removeRouteManualEndpoint(String endpoint) {
        routeConnections.disconnect(endpoint);
    }

    void addHandlerGroup(String groupName) {
        if (groupName == null || groupName.isBlank()) {
            throw new ZLinkConfigurationException(
                "channel handler group name is required: " + name);
        }
        handlerGroups.add(groupName);
    }

    void addSendHandler(ChannelSendHandlerRegistration handler) {
        requireNonBlankPacketName(handler.packetName(),
            "client/server channel send handler packet name");
        clientServer.sendHandlers.add(handler.withPacketName(
            resolvePacketName(handler.messageType(), handler.packetName())));
    }

    void addRequestHandler(ChannelRequestHandlerRegistration handler) {
        requireNonBlankPacketName(handler.packetName(),
            "client/server channel request handler packet name");
        clientServer.requestHandlers.add(handler.withPacketName(
            resolvePacketName(handler.requestType(), handler.packetName())));
    }

    void addPublishHandler(ChannelPublishHandlerRegistration handler) {
        requireNonBlankPacketName(handler.packetName(),
            "fanout channel publish handler packet name");
        fanout.publishHandlers.add(handler.withPacketName(
            resolvePacketName(handler.messageType(), handler.packetName())));
    }

    void addRouteRequestHandler(ChannelRouteRequestHandlerRegistration handler) {
        requireNonBlankPacketName(handler.packetName(),
            "route mesh request handler packet name");
        routeMesh.requestHandlers.add(handler.withPacketName(
            resolvePacketName(handler.requestType(), handler.packetName())));
    }

    void addRouteSendHandler(ChannelRouteSendHandlerRegistration handler) {
        requireNonBlankPacketName(handler.packetName(),
            "route mesh send handler packet name");
        routeMesh.sendHandlers.add(handler.withPacketName(
            resolvePacketName(handler.messageType(), handler.packetName())));
    }

    public void validate(boolean locationAutoConnectEnabled) {
        validate(locationAutoConnectEnabled, new ZLinkScannedHandlerCatalog(List.of()));
    }

    public void validate(
        boolean locationAutoConnectEnabled,
        ZLinkScannedHandlerCatalog handlerCatalog) {
        if (kind == ChannelKind.CLIENT_SERVER) {
            validateClientServer(locationAutoConnectEnabled, handlerCatalog);
        } else if (kind == ChannelKind.FANOUT) {
            validateFanout(locationAutoConnectEnabled, handlerCatalog);
        } else if (kind == ChannelKind.ROUTE_MESH) {
            validateRouteMesh(locationAutoConnectEnabled, handlerCatalog);
        }
    }

    /**
     * Validates the listener limit required by a host-wide application HWM.
     * This method is called during registration validation; it is not a
     * runtime socket mutation API.
     */
    public void validateApplicationHwm(long applicationHwmBytes) {
        if (applicationHwmBytes == 0 || !clientServer.serverEnabled) {
            return;
        }
        if (serverSocketOptions.maxMessageSize() <= 0) {
            throw new ZLinkConfigurationException(
                "Application HWM requires a finite positive MaxMessageSize on channel: "
                    + name);
        }
    }

    private void validateClientServer(
        boolean locationAutoConnectEnabled,
        ZLinkScannedHandlerCatalog handlerCatalog) {
        if (clientServer.duplicateClientDeclaration) {
            throw new ZLinkConfigurationException(
                "duplicate client/server client role: " + name);
        }
        if (clientServer.duplicateServerDeclaration) {
            throw new ZLinkConfigurationException(
                "duplicate client/server server role: " + name);
        }
        if (clientServer.clientEnabled
            && !locationAutoConnectEnabled
            && !clientServer.serverEnabled
            && clientServer.manualEndpoints.isEmpty()) {
            throw new ZLinkConfigurationException(
                "client/server channel client requires location auto-connect or manual connections: " + name);
        }
        if (clientServer.serverEnabled && clientServer.serverBinds.isEmpty()) {
            throw new ZLinkConfigurationException(
                "client/server channel server requires at least one bind endpoint: " + name);
        }
        validateMappedGroups(handlerCatalog, ZLinkScannedHandlerSurface.CHANNEL,
            Set.of(ZLinkScannedHandlerKind.SEND, ZLinkScannedHandlerKind.REQUEST));
        boolean hasMappedHandlers =
            hasMappedHandler(handlerCatalog, ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.SEND)
                || hasMappedHandler(handlerCatalog, ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.REQUEST);
        if (clientServer.serverEnabled
            && clientServer.requestHandlers.isEmpty()
            && clientServer.sendHandlers.isEmpty()
            && !hasMappedHandlers) {
            throw new ZLinkConfigurationException(
                "client/server channel server requires a send/request handler or handler group: " + name);
        }
        Set<String> packetNames = mappedPacketNames(
            handlerCatalog,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.SEND,
            "duplicate client/server send handler packet name");
        for (ChannelSendHandlerRegistration handler : clientServer.sendHandlers) {
            if (!packetNames.add(handler.packetName())) {
                throw new ZLinkConfigurationException(
                    "duplicate client/server send handler packet name: "
                        + name + "/" + handler.packetName());
            }
        }
        packetNames = mappedPacketNames(
            handlerCatalog,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.REQUEST,
            "duplicate client/server request handler packet name");
        for (ChannelRequestHandlerRegistration handler : clientServer.requestHandlers) {
            if (!packetNames.add(handler.packetName())) {
                throw new ZLinkConfigurationException(
                    "duplicate client/server request handler packet name: "
                        + name + "/" + handler.packetName());
            }
        }
    }

    private void validateFanout(
        boolean locationAutoConnectEnabled,
        ZLinkScannedHandlerCatalog handlerCatalog) {
        if (fanout.publisherEnabled && fanout.publisherBinds.isEmpty()) {
            throw new ZLinkConfigurationException(
                "fanout channel publisher requires at least one bind endpoint: " + name);
        }
        if (fanout.subscriberEnabled
            && fanout.automaticSubscriberEnabled
            && !fanout.subscriberManualEndpoints.isEmpty()) {
            throw new ZLinkConfigurationException(
                "fanout channel cannot combine automatic subscriber discovery "
                    + "with manual subscriber connections: " + name);
        }
        if (fanout.subscriberEnabled
            && ((fanout.automaticSubscriberEnabled
                    && !locationAutoConnectEnabled)
                || (!fanout.automaticSubscriberEnabled
                    && fanout.subscriberManualEndpoints.isEmpty()))) {
            throw new ZLinkConfigurationException(
                "fanout channel subscriber requires location auto-connect or manual connections: " + name);
        }
        validateMappedGroups(handlerCatalog, ZLinkScannedHandlerSurface.CHANNEL,
            Set.of(ZLinkScannedHandlerKind.PUBLISH));
        boolean hasMappedPublishHandlers =
            hasMappedHandler(handlerCatalog, ZLinkScannedHandlerSurface.CHANNEL, ZLinkScannedHandlerKind.PUBLISH);
        if (fanout.subscriberEnabled
            && fanout.publishHandlers.isEmpty()
            && !hasMappedPublishHandlers) {
            throw new ZLinkConfigurationException(
                "fanout channel subscriber requires a publish handler or handler group: " + name);
        }
        Set<String> packetNames = mappedPacketNames(
            handlerCatalog,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.PUBLISH,
            "duplicate fanout publish handler packet name");
        for (ChannelPublishHandlerRegistration handler : fanout.publishHandlers) {
            if (!packetNames.add(handler.packetName())) {
                throw new ZLinkConfigurationException(
                    "duplicate fanout publish handler packet name: "
                        + name + "/" + handler.packetName());
            }
        }
    }

    private void validateRouteMesh(
        boolean locationAutoConnectEnabled,
        ZLinkScannedHandlerCatalog handlerCatalog) {
        if (routeMesh.binds.isEmpty() && !routeMesh.clientEnabled) {
            throw new ZLinkConfigurationException(
                "route mesh channel must enable server or client capability: " + name);
        }
        if (routeMesh.clientEnabled
            && !locationAutoConnectEnabled
            && routeMesh.manualEndpoints.isEmpty()) {
            throw new ZLinkConfigurationException(
                "route mesh channel requires location auto-connect or manual connections: " + name);
        }
        validateMappedGroups(handlerCatalog, ZLinkScannedHandlerSurface.ROUTE,
            Set.of(ZLinkScannedHandlerKind.SEND, ZLinkScannedHandlerKind.REQUEST));
        Set<String> packetNames = mappedPacketNames(
            handlerCatalog,
            ZLinkScannedHandlerSurface.ROUTE,
            ZLinkScannedHandlerKind.SEND,
            "duplicate route mesh send handler packet name");
        for (ChannelRouteSendHandlerRegistration handler : routeMesh.sendHandlers) {
            if (!packetNames.add(handler.packetName())) {
                throw new ZLinkConfigurationException(
                    "duplicate route mesh send handler packet name: "
                        + name + "/" + handler.packetName());
            }
        }
        addMappedPacketNames(
            packetNames,
            handlerCatalog,
            ZLinkScannedHandlerSurface.ROUTE,
            ZLinkScannedHandlerKind.REQUEST,
            "duplicate route mesh request handler packet name");
        for (ChannelRouteRequestHandlerRegistration handler : routeMesh.requestHandlers) {
            if (!packetNames.add(handler.packetName())) {
                throw new ZLinkConfigurationException(
                    "duplicate route mesh request handler packet name: "
                        + name + "/" + handler.packetName());
            }
        }
    }

    static String requireEndpointValue(String endpoint) {
        if (endpoint == null || endpoint.isBlank()) {
            throw new ZLinkConfigurationException("endpoint is required");
        }
        return endpoint;
    }

    private static String requireEndpoint(String endpoint) {
        return requireEndpointValue(endpoint);
    }

    private void requireNonBlankPacketName(String packetName, String label) {
        if (packetName != null && packetName.isBlank()) {
            throw new ZLinkConfigurationException(label + " is required: " + name);
        }
    }

    private static String resolvePacketName(Class<?> messageType, String explicitPacketName) {
        return ZLinkPacketNames.resolve(messageType, explicitPacketName);
    }

    private static String resolvePacketName(Class<?> messageType) {
        return ZLinkPacketNames.resolve(messageType);
    }

    private void validateMappedGroups(
        ZLinkScannedHandlerCatalog handlerCatalog,
        ZLinkScannedHandlerSurface surface,
        Set<ZLinkScannedHandlerKind> allowedKinds) {
        for (String group : handlerGroups) {
            if (!handlerCatalog.containsGroup(group)) {
                throw new ZLinkConfigurationException(
                    "channel maps unknown handler group: " + name + "/" + group);
            }
            if (!handlerCatalog.groupHasOnly(group, surface, allowedKinds)) {
                throw new ZLinkConfigurationException(
                    "channel maps incompatible handler group: " + name + "/" + group);
            }
        }
    }

    private boolean hasMappedHandler(
        ZLinkScannedHandlerCatalog handlerCatalog,
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind) {
        return !handlerCatalog.matching(Set.copyOf(handlerGroups), surface, kind).isEmpty();
    }

    private Set<String> mappedPacketNames(
        ZLinkScannedHandlerCatalog handlerCatalog,
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind,
        String label) {
        Set<String> packetNames = new HashSet<>();
        addMappedPacketNames(packetNames, handlerCatalog, surface, kind, label);
        return packetNames;
    }

    private void addMappedPacketNames(
        Set<String> packetNames,
        ZLinkScannedHandlerCatalog handlerCatalog,
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind,
        String label) {
        for (var handler : handlerCatalog.matching(Set.copyOf(handlerGroups), surface, kind)) {
            if (!packetNames.add(handler.packetName())) {
                throw new ZLinkConfigurationException(
                    label + ": " + name + "/" + handler.packetName());
            }
        }
    }

    private static final class ClientServerState {
        private final List<String> serverBinds = new ArrayList<>();
        private String advertiseHost;
        private final List<String> manualEndpoints = new ArrayList<>();
        private final List<ChannelSendHandlerRegistration> sendHandlers = new ArrayList<>();
        private final List<ChannelRequestHandlerRegistration> requestHandlers = new ArrayList<>();
        private boolean clientEnabled;
        private boolean serverEnabled;
        private boolean clientDeclared;
        private boolean serverDeclared;
        private boolean duplicateClientDeclaration;
        private boolean duplicateServerDeclaration;
    }

    private static final class FanoutState {
        private final List<String> publisherBinds = new ArrayList<>();
        private String advertiseHost;
        private final List<String> subscriberManualEndpoints = new ArrayList<>();
        private final List<ChannelPublishHandlerRegistration> publishHandlers = new ArrayList<>();
        private boolean publisherEnabled;
        private boolean subscriberEnabled;
        private boolean automaticSubscriberEnabled;
    }

    private static final class RouteMeshState {
        private final List<String> binds = new ArrayList<>();
        private final List<String> manualEndpoints = new ArrayList<>();
        private final List<ChannelRouteSendHandlerRegistration> sendHandlers = new ArrayList<>();
        private final List<ChannelRouteRequestHandlerRegistration> requestHandlers = new ArrayList<>();
        private boolean clientEnabled;
    }

}
