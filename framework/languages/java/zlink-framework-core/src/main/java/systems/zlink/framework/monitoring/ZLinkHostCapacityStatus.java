package systems.zlink.framework.monitoring;

import java.util.Objects;

/** Coherent host capacity projection for one measurement epoch. */
public record ZLinkHostCapacityStatus(
    long measurementEpoch,
    ZLinkCoreHwmStatus coreHwm,
    ZLinkApplicationJobQueueStatus applicationJobQueue) {
    public ZLinkHostCapacityStatus {
        Objects.requireNonNull(coreHwm, "coreHwm");
        Objects.requireNonNull(applicationJobQueue, "applicationJobQueue");
    }
}
