package systems.zlink.framework.configuration;

/**
 * Ratio Auto sizing applies to the detected process memory limit when it
 * computes the host-wide Application HWM.
 */
public enum ZLinkApplicationHwmProfile {
    COMPACT,
    LOW_LATENCY,
    BALANCED,
    THROUGHPUT
}
