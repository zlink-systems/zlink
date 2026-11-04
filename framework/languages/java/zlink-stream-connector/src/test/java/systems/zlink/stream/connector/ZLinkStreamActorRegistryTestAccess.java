package systems.zlink.stream.connector;

/** Test seam for Kotlin coverage of the connector-owned Actor registry. */
public final class ZLinkStreamActorRegistryTestAccess {
    private ZLinkStreamActorRegistryTestAccess() {}

    public static void bound(ZLinkStreamConnector connector, byte[] payload) {
        ((DefaultZLinkStreamConnector) connector).actorRegistry.bound(payload);
    }

    public static void unbound(ZLinkStreamConnector connector, byte[] payload) {
        ((DefaultZLinkStreamConnector) connector).actorRegistry.unbound(payload);
    }
}
