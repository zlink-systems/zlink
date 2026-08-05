package systems.zlink.framework.runtime.host;

/** Internal classification used while adapting one-way admission to public exceptions. */
enum ZLinkOneWayAdmissionStatus {
    SUBMITTED,
    BACKPRESSURED,
    TIMED_OUT,
    ROUTE_NOT_CONNECTED,
    TARGET_NOT_FOUND,
    SHUTDOWN
}
