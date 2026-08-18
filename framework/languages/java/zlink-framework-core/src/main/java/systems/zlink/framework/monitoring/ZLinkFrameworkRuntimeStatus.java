package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.Objects;
import java.util.Optional;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult;

/**
 * {@code safeToShutdown} is the source-published observation of spec 30 §11:
 * every relocation unit this source started has reached its Message Follow
 * route removal point (S4) and its cutover retransmission window has ended.
 * It is a source-local observation, never a completion ACK from a target.
 */
public record ZLinkFrameworkRuntimeStatus(
    ZLinkFrameworkRuntimeState state,
    boolean isReady,
    boolean acceptingWork,
    Optional<Instant> deadline,
    Optional<ZLinkFrameworkRelocationResult> relocationResult,
    Optional<ZLinkFrameworkTerminationResult> terminationResult,
    boolean safeToShutdown,
    ZLinkHostCapacityStatus capacity,
    long sequence,
    Instant observedAt) {
    public ZLinkFrameworkRuntimeStatus {
        Objects.requireNonNull(state, "state");
        deadline = deadline == null ? Optional.empty() : deadline;
        relocationResult =
            relocationResult == null ? Optional.empty() : relocationResult;
        terminationResult =
            terminationResult == null ? Optional.empty() : terminationResult;
        Objects.requireNonNull(capacity, "capacity");
        Objects.requireNonNull(observedAt, "observedAt");
    }
}
