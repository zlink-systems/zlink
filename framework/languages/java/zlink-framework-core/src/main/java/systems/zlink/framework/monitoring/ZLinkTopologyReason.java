package systems.zlink.framework.monitoring;

public enum ZLinkTopologyReason {
    RUNTIME_NOT_READY,
    NO_READY_PEER,
    NO_READY_TARGET,
    LOCATION_UNAVAILABLE,
    CAPACITY_EXCEEDED,
    DRAINING,
    INTERNAL_FAILURE
}
