package systems.zlink.framework.runtime.internal.locations;

/** Internal topology category used while reconciling automatic connections. */
public enum ZLinkAutoConnectType {
    ROUTE_MESH,
    DEALER_MESH,
    SPOT_MESH,
    CLIENT_SERVER,
    FANOUT
}
