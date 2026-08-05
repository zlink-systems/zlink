package systems.zlink.framework.monitoring;

import java.util.Objects;

/** A complete monitoring snapshot and the loss counters for its subscription. */
public record ZLinkObservedStatus<T>(
    T status,
    ZLinkObservationLoss loss) {
    public ZLinkObservedStatus {
        Objects.requireNonNull(status, "status");
        Objects.requireNonNull(loss, "loss");
    }
}
