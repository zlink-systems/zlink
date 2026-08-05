package systems.zlink.framework.runtime.channels;

import java.time.Duration;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ClientServerChannelBuilder;
import systems.zlink.framework.configuration.FanoutChannelBuilder;
import systems.zlink.framework.runtime.internal.configuration.RouteMeshChannelBuilder;
import systems.zlink.framework.configuration.ZLinkClientServerChannelClientBuilder;
import systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder;
import systems.zlink.framework.configuration.ZLinkEndpointConnections;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;

public final class ChannelBuilders {
    private ChannelBuilders() {
    }

    public static ClientServerChannelBuilder clientServer(ChannelRegistration registration) {
        return clientServer(registration, "127.0.0.1", null);
    }

    public static ClientServerChannelBuilder clientServer(
        ChannelRegistration registration,
        String bindHost,
        String advertiseHost) {
        return new ClientServer(registration, bindHost, advertiseHost);
    }

    public static FanoutChannelBuilder fanout(ChannelRegistration registration) {
        return fanout(registration, "127.0.0.1", null);
    }

    public static FanoutChannelBuilder fanout(
        ChannelRegistration registration,
        String bindHost,
        String advertiseHost) {
        return new Fanout(registration, bindHost, advertiseHost);
    }

    public static RouteMeshChannelBuilder routeMesh(ChannelRegistration registration) {
        return new RouteMesh(registration);
    }

    private record ClientServer(
        ChannelRegistration registration,
        String bindHost,
        String advertiseHost) implements ClientServerChannelBuilder {
        @Override
        public ZLinkClientServerChannelClientBuilder client() {
            registration.declareClient();
            return new ClientServerClient(registration);
        }

        @Override
        public ZLinkClientServerChannelServerBuilder server() {
            registration.declareServer();
            return new ClientServerServer(registration, bindHost, advertiseHost);
        }
    }

    private record ClientServerClient(ChannelRegistration registration)
        implements ZLinkClientServerChannelClientBuilder {
        @Override
        public ZLinkClientServerChannelClientBuilder connect(String endpoint) {
            registration.addClientManualEndpoint(endpoint);
            return this;
        }
    }

    private static final class ClientServerServer
        implements ZLinkClientServerChannelServerBuilder {
        private final ChannelRegistration registration;
        private String bindHost;
        private String advertiseHost;
        private Integer listenPort;

        private ClientServerServer(
            ChannelRegistration registration,
            String bindHost,
            String advertiseHost) {
            this.registration = registration;
            this.bindHost = bindHost;
            this.advertiseHost = advertiseHost;
            if (advertiseHost != null) {
                registration.setClientServerAdvertiseHost(advertiseHost);
            }
        }

        @Override
        public ZLinkClientServerChannelServerBuilder listen() {
            return listen(0);
        }

        @Override
        public ZLinkClientServerChannelServerBuilder listen(int port) {
            if (port < 0 || port > 65_535) {
                throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                    "ClientServer listen port must be between 0 and 65535.");
            }
            listenPort = port;
            applyListen();
            return this;
        }

        @Override
        public ZLinkClientServerChannelServerBuilder setBindHost(String host) {
            bindHost = requireHost(host, "bind host");
            applyListen();
            return this;
        }

        @Override
        public ZLinkClientServerChannelServerBuilder setAdvertiseHost(String host) {
            advertiseHost = requireHost(host, "advertise host");
            registration.setClientServerAdvertiseHost(advertiseHost);
            return this;
        }

        @Override
        public ZLinkClientServerChannelServerBuilder setWeight(int weight) {
            registration.serverSocketOptions().weight(weight);
            return this;
        }

        @Override
        public ZLinkClientServerChannelServerBuilder addHandlerGroup(String groupName) {
            registration.addHandlerGroup(groupName);
            return this;
        }

        @Override
        public <THandler extends ZLinkSendHandler<TMessage>, TMessage>
        ZLinkClientServerChannelServerBuilder addSendHandler(
            Class<THandler> handlerType,
            Class<TMessage> messageType) {
            registration.addSendHandler(new ChannelSendHandlerRegistration(
                handlerType,
                messageType,
                null));
            return this;
        }

        @Override
        public <THandler extends ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply>
        ZLinkClientServerChannelServerBuilder addRequestHandler(
            Class<THandler> handlerType,
            Class<TRequest> requestType,
            Class<TReply> replyType) {
            registration.addRequestHandler(new ChannelRequestHandlerRegistration(
                handlerType,
                requestType,
                replyType,
                null));
            return this;
        }

        private void applyListen() {
            if (listenPort != null) {
                registration.replaceClientServerBind(
                    "tcp://" + bindHost + ":" + listenPort);
            }
        }

        private static String requireHost(String host, String label) {
            if (host == null || host.isBlank()) {
                throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                    "ClientServer " + label + " must not be empty.");
            }
            return host;
        }
    }

    private static final class Fanout implements FanoutChannelBuilder {
        private final ChannelRegistration registration;
        private String bindHost;
        private String advertiseHost;
        private Integer listenPort;

        private Fanout(
            ChannelRegistration registration,
            String bindHost,
            String advertiseHost) {
            this.registration = registration;
            this.bindHost = bindHost;
            this.advertiseHost = advertiseHost;
            if (advertiseHost != null) {
                registration.setFanoutAdvertiseHost(advertiseHost);
            }
        }

        @Override
        public FanoutChannelBuilder enablePublisher(String endpoint) {
            registration.enablePublisher();
            registration.addPublisherBind(endpoint);
            return this;
        }

        @Override
        public FanoutChannelBuilder enablePublisher() {
            return enablePublisher(0);
        }

        @Override
        public FanoutChannelBuilder enablePublisher(int port) {
            if (port < 0 || port > 65_535) {
                throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                    "Fanout listen port must be between 0 and 65535.");
            }
            listenPort = port;
            registration.enablePublisher();
            applyListen();
            return this;
        }

        @Override
        public FanoutChannelBuilder setBindHost(String host) {
            bindHost = ClientServerServer.requireHost(host, "bind host");
            applyListen();
            return this;
        }

        @Override
        public FanoutChannelBuilder setAdvertiseHost(String host) {
            advertiseHost = ClientServerServer.requireHost(host, "advertise host");
            registration.setFanoutAdvertiseHost(advertiseHost);
            return this;
        }

        private void applyListen() {
            if (listenPort != null) {
                registration.replacePublisherBind(
                    "tcp://" + bindHost + ":" + listenPort);
            }
        }

        @Override
        public FanoutChannelBuilder setRoutingId(RoutingId routingId) {
            registration.setRoutingId(routingId);
            return this;
        }

        @Override
        public FanoutChannelBuilder setRoutingIdPrefix(String prefix) {
            registration.setRoutingIdPrefix(prefix);
            return this;
        }

        @Override
        public FanoutChannelBuilder enableSubscriber() {
            registration.enableAutomaticSubscriber();
            return this;
        }

        @Override
        public FanoutChannelBuilder connect(String endpoint) {
            registration.enableSubscriber();
            registration.addSubscriberManualEndpoint(endpoint);
            return this;
        }

        @Override
        public ZLinkEndpointConnections subscriberConnections() {
            return registration.subscriberConnections();
        }

        @Override
        public FanoutChannelBuilder addHandlerGroup(String groupName) {
            registration.addHandlerGroup(groupName);
            return this;
        }

        @Override
        public void addPublishHandler(
            Class<?> handlerType,
            Class<?> messageType) {
            addPublishHandler(handlerType, messageType, null);
        }

        @Override
        public void addPublishHandler(
            Class<?> handlerType,
            Class<?> messageType,
            String packetName) {
            registration.addPublishHandler(new ChannelPublishHandlerRegistration(
                handlerType,
                messageType,
                packetName));
        }

        @Override
        @SuppressWarnings({"unchecked", "rawtypes"})
        public FanoutChannelBuilder addPublishHandler(Class<?> handlerType) {
            addPublishHandler(handlerType, (String) null);
            return this;
        }

        @Override
        @SuppressWarnings({"unchecked", "rawtypes"})
        public FanoutChannelBuilder addPublishHandler(Class<?> handlerType, String packetName) {
            registration.addPublishHandler(new ChannelPublishHandlerRegistration(
                handlerType,
                String.class,
                packetName));
            return this;
        }
    }

    private record RouteMesh(ChannelRegistration registration) implements RouteMeshChannelBuilder {
        @Override
        public RouteMeshChannelBuilder enableServer(String endpoint) {
            registration.addRouteBind(endpoint);
            return this;
        }

        @Override
        public ZLinkSocketRuntimeOptions configureServerSocket() {
            return registration.serverSocketOptions();
        }

        @Override
        public RouteMeshChannelBuilder setRoutingId(RoutingId routingId) {
            registration.setRouteRoutingId(routingId);
            return this;
        }

        @Override
        public RouteMeshChannelBuilder setDefaultRequestTimeout(Duration timeout) {
            registration.setDefaultRequestTimeout(timeout);
            return this;
        }

        @Override
        public RouteMeshChannelBuilder enableClient() {
            registration.enableClient();
            return this;
        }

        @Override
        public RouteMeshChannelBuilder enableClient(String endpoint) {
            registration.enableClient();
            registration.addRouteManualEndpoint(endpoint);
            return this;
        }

        @Override
        public ZLinkEndpointConnections clientConnections() {
            return registration.routeConnections();
        }

        @Override
        public RouteMeshChannelBuilder addHandlerGroup(String groupName) {
            registration.addHandlerGroup(groupName);
            return this;
        }

        @Override
        public void addSendHandler(
            Class<?> handlerType,
            Class<?> messageType) {
            addSendHandler(handlerType, messageType, null);
        }

        @Override
        public void addSendHandler(
            Class<?> handlerType,
            Class<?> messageType,
            String packetName) {
            registration.addRouteSendHandler(new ChannelRouteSendHandlerRegistration(
                handlerType,
                messageType,
                packetName));
        }

        @Override
        public void addRequestHandler(
            Class<?> handlerType,
            Class<?> requestType,
            Class<?> replyType) {
            addRequestHandler(handlerType, requestType, replyType, null);
        }

        @Override
        public void addRequestHandler(
            Class<?> handlerType,
            Class<?> requestType,
            Class<?> replyType,
            String packetName) {
            registration.addRouteRequestHandler(new ChannelRouteRequestHandlerRegistration(
                handlerType,
                requestType,
                replyType,
                packetName));
        }

    }

}
