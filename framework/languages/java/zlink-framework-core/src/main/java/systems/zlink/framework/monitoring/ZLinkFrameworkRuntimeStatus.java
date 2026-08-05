package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.Objects;
import java.util.Optional;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult;

public record ZLinkFrameworkRuntimeStatus(
    ZLinkFrameworkRuntimeState state,
    boolean isReady,
    boolean acceptingWork,
    Optional<Instant> deadline,
    Optional<ZLinkFrameworkRelocationResult> relocationResult,
    Optional<ZLinkFrameworkTerminationResult> terminationResult,
    ZLinkInboundDispatchStatus inboundDispatch,
    long sequence,
    Instant observedAt) {
    public ZLinkFrameworkRuntimeStatus {
        Objects.requireNonNull(state, "state");
        deadline = deadline == null ? Optional.empty() : deadline;
        relocationResult =
            relocationResult == null ? Optional.empty() : relocationResult;
        terminationResult =
            terminationResult == null ? Optional.empty() : terminationResult;
        Objects.requireNonNull(inboundDispatch, "inboundDispatch");
        Objects.requireNonNull(observedAt, "observedAt");
    }
}
