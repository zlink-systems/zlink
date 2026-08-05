package systems.zlink.framework.configuration;

import systems.zlink.framework.streams.ZLinkSession;

public interface ZLinkStreamNodeBuilder {
    ZLinkStreamNodeBuilder bind(String endpoint);

    ZLinkStreamNodeBuilder bind();

    ZLinkStreamNodeBuilder bind(int port);

    ZLinkStreamNodeBuilder setBindHost(String host);

    ZLinkStreamNodeBuilder setAdvertiseHost(String host);

    ZLinkStreamSocketConfig configureSocket();

    ZLinkStreamNodeBuilder setTlsServer(String certificatePath, String keyPath);

    ZLinkStreamNodeBuilder setTlsServer(
        String certificatePath,
        String keyPath,
        boolean requireClientCertificate);

    ZLinkStreamNodeBuilder registerSession(Class<? extends ZLinkSession> sessionType);

    ZLinkStreamNodeBuilder enableActorDispatch();

    ZLinkStreamNodeBuilder addSessionPacketHandler(Class<?> handlerType);
}
