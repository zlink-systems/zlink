package systems.zlink.framework.configuration;

/** Startup profile for the host-wide Application Job Queue. */
public enum ZLinkApplicationJobQueueProfile {
    COMPACT,
    LOW_LATENCY,
    BALANCED,
    THROUGHPUT
}
