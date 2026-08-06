package systems.zlink.framework.monitoring;

/** Identifies the listener family addressed by a listener status query. */
public enum ZLinkListenerKind {
    ROUTE_MESH,
    CLIENT_SERVER,
    FANOUT,
    STREAM
}
