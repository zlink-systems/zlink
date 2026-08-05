package systems.zlink.stream.connector;

public final class ZLinkStreamConnectorFactory {
    private ZLinkStreamConnectorFactory() {
    }

    public static ZLinkStreamConnector create(ZLinkStreamConnectorOptions options) {
        return new DefaultZLinkStreamConnector(options);
    }
}
