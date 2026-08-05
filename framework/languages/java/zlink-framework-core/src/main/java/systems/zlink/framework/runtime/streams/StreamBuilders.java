package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder;
import systems.zlink.framework.configuration.ZLinkStreamSocketConfig;
import systems.zlink.framework.streams.ZLinkSession;

public final class StreamBuilders {
    private StreamBuilders() {
    }

    public static ZLinkStreamNodeBuilder streamNode(StreamNodeRegistration registration) {
        return streamNode(registration, "127.0.0.1", null);
    }

    public static ZLinkStreamNodeBuilder streamNode(
        StreamNodeRegistration registration,
        String bindHost,
        String advertiseHost) {
        return new StreamNode(registration, bindHost, advertiseHost);
    }

    private static final class StreamNode implements ZLinkStreamNodeBuilder {
        private final StreamNodeRegistration registration;
        private String bindHost;
        private Integer listenPort;

        private StreamNode(
            StreamNodeRegistration registration,
            String bindHost,
            String advertiseHost) {
            this.registration = registration;
            this.bindHost = bindHost;
            registration.setAdvertiseHost(advertiseHost);
        }

        @Override
        public ZLinkStreamNodeBuilder bind(String endpoint) {
            registration.bind(endpoint);
            return this;
        }

        @Override
        public ZLinkStreamNodeBuilder bind() {
            return bind(0);
        }

        @Override
        public ZLinkStreamNodeBuilder bind(int port) {
            if (port < 0 || port > 65_535) {
                throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                    "stream listen port must be between 0 and 65535");
            }
            listenPort = port;
            applyBind();
            return this;
        }

        @Override
        public ZLinkStreamNodeBuilder setBindHost(String host) {
            bindHost = requireHost(host, "bind host");
            applyBind();
            return this;
        }

        @Override
        public ZLinkStreamNodeBuilder setAdvertiseHost(String host) {
            registration.setAdvertiseHost(requireHost(host, "advertise host"));
            return this;
        }

        @Override
        public ZLinkStreamSocketConfig configureSocket() {
            return registration.socketConfig();
        }

        private void applyBind() {
            if (listenPort != null) {
                registration.replaceBind(
                    "tcp://" + bindHost + ":" + listenPort);
            }
        }

        private static String requireHost(String host, String label) {
            if (host == null || host.isBlank()) {
                throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                    "stream " + label + " must not be empty");
            }
            return host;
        }

        @Override
        public ZLinkStreamNodeBuilder setTlsServer(String certificatePath, String keyPath) {
            registration.setTlsServer(certificatePath, keyPath, false);
            return this;
        }

        @Override
        public ZLinkStreamNodeBuilder setTlsServer(
            String certificatePath,
            String keyPath,
            boolean requireClientCertificate) {
            registration.setTlsServer(certificatePath, keyPath, requireClientCertificate);
            return this;
        }

        @Override
        public ZLinkStreamNodeBuilder registerSession(Class<? extends ZLinkSession> sessionType) {
            registration.registerSession(sessionType);
            return this;
        }

        @Override
        public ZLinkStreamNodeBuilder enableActorDispatch() {
            registration.enableActorDispatch();
            return this;
        }

        @Override
        public ZLinkStreamNodeBuilder addSessionPacketHandler(Class<?> handlerType) {
            registration.addSessionPacketHandler(handlerType);
            return this;
        }
    }
}
