package systems.zlink.framework.runtime.channels;

import java.util.function.BiConsumer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;

final class ZLinkChannelRuntimeConfigurator {
    private final ZLinkChannelBackendAdapter backend;
    private final ZLinkBackendContext context;
    private final ZLinkChannelSocketRegistry sockets;
    private final ZLinkChannelDispatchRegistry dispatchRegistry;
    private final ZLinkChannelHandlerCatalog handlers;
    private final BiConsumer<String, ZLinkBackendRouterSocket> startRequestLoop;
    private final BiConsumer<String, ZLinkBackendRouterSocket> startRouteLoop;
    private final BiConsumer<String, ZLinkBackendSubscriberSocket> startSubscribeLoop;
    private final boolean dedicatedClientServerRuntime;

    ZLinkChannelRuntimeConfigurator(
        ZLinkChannelBackendAdapter backend,
        ZLinkBackendContext context,
        ZLinkChannelSocketRegistry sockets,
        ZLinkChannelDispatchRegistry dispatchRegistry,
        ZLinkChannelHandlerCatalog handlers,
        BiConsumer<String, ZLinkBackendRouterSocket> startRequestLoop,
        BiConsumer<String, ZLinkBackendRouterSocket> startRouteLoop,
        BiConsumer<String, ZLinkBackendSubscriberSocket> startSubscribeLoop,
        boolean dedicatedClientServerRuntime) {
        this.backend = backend;
        this.context = context;
        this.sockets = sockets;
        this.dispatchRegistry = dispatchRegistry;
        this.handlers = handlers;
        this.startRequestLoop = startRequestLoop;
        this.startRouteLoop = startRouteLoop;
        this.startSubscribeLoop = startSubscribeLoop;
        this.dedicatedClientServerRuntime = dedicatedClientServerRuntime;
    }

    void configure(ChannelRegistration channel) {
        if (channel.kind() == ChannelKind.CLIENT_SERVER) {
            configureClientServer(channel);
        } else if (channel.kind() == ChannelKind.FANOUT) {
            configureFanout(channel);
        } else if (channel.kind() == ChannelKind.ROUTE_MESH) {
            configureRouteMesh(channel);
        }
    }

    private void configureClientServer(ChannelRegistration channel) {
        if (channel.clientEnabled() && !dedicatedClientServerRuntime) {
            ZLinkBackendDealerSocket dealer = backend.createDealerSocket(context);
            dealer.setChannelName(channel.name());
            sockets.registerClient(channel.name(), dealer);
        }
        if (channel.serverBinds().isEmpty()) {
            return;
        }
        ZLinkBackendRouterSocket router = backend.createRouterSocket(context);
        router.setChannelName(channel.name());
        systems.zlink.contracts.core.RoutingId serverRoutingId =
            channel.routingId() == null
                ? systems.zlink.contracts.core.RoutingId.from(
                    java.util.UUID.randomUUID())
                : channel.routingId();
        router.setRoutingId(serverRoutingId);
        applyServerSocketOptions(channel, router);
        for (String endpoint : channel.serverBinds()) {
            router.bind(endpoint);
        }
        sockets.registerServer(channel.name(), serverRoutingId, router);
        dispatchRegistry.registerClientServer(
            channel.name(),
            handlers.sendHandlers(channel),
            handlers.requestHandlers(channel));
        startRequestLoop.accept(channel.name(), router);
    }

    private void configureFanout(ChannelRegistration channel) {
        if (channel.publisherEnabled()) {
            ZLinkBackendPublisherSocket publisher = backend.createPublisherSocket(context);
            publisher.setChannelName(channel.name());
            systems.zlink.contracts.core.RoutingId publisherRoutingId =
                channel.routingId() == null
                    ? systems.zlink.contracts.core.RoutingId.from(
                        channel.routingIdPrefix() + "-" + java.util.UUID.randomUUID())
                    : channel.routingId();
            publisher.setRoutingId(publisherRoutingId);
            for (String endpoint : channel.publisherBinds()) {
                publisher.bind(endpoint);
            }
            sockets.registerPublisher(
                channel.name(), publisherRoutingId, publisher);
        }
        if (!channel.subscriberEnabled()) {
            return;
        }
        if (channel.automaticSubscriberEnabled()) {
            dispatchRegistry.registerFanout(
                channel.name(),
                handlers.publishHandlers(channel));
            return;
        }
        ZLinkBackendSubscriberSocket subscriber = backend.createSubscriberSocket(context);
        subscriber.setChannelName(channel.name());
        channel.subscriberConnections().attach(subscriber);
        subscriber.setSubscription("");
        sockets.registerSubscriber(channel.name(), subscriber);
        dispatchRegistry.registerFanout(
            channel.name(),
            handlers.publishHandlers(channel));
        startSubscribeLoop.accept(channel.name(), subscriber);
    }

    private void configureRouteMesh(ChannelRegistration channel) {
        ZLinkBackendRouterSocket router = backend.createRouterSocket(context);
        router.setChannelName(channel.name());
        if (channel.routeRoutingId() != null) {
            router.setRoutingId(channel.routeRoutingId());
        }
        applyServerSocketOptions(channel, router);
        channel.routeConnections().attach(router);
        for (String endpoint : channel.routeBinds()) {
            router.bind(endpoint);
        }
        sockets.registerRouteRouter(channel.name(), router);
        dispatchRegistry.registerRoute(
            channel.name(),
            handlers.routeSendHandlers(channel),
            handlers.routeRequestHandlers(channel));
        startRouteLoop.accept(channel.name(), router);
    }

    private static void applyServerSocketOptions(
        ChannelRegistration channel,
        ZLinkBackendRouterSocket router) {
        var options = channel.serverSocketOptions();
        if (options.maxMessageSize() > 0) {
            router.setMaxMessageSize(options.maxMessageSize());
        }
        router.setPeerWeight(options.weight());
    }
}
