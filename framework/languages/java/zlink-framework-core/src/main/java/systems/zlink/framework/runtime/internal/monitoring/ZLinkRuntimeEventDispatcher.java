package systems.zlink.framework.runtime.internal.monitoring;

import java.util.Objects;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;

public final class ZLinkRuntimeEventDispatcher {
    private static final Logger LOGGER =
        LoggerFactory.getLogger("systems.zlink.framework.runtime");

    public void publishObserverFailure(
        String sourceName,
        String callbackName,
        Throwable failure) {
        Throwable actual = Objects.requireNonNull(failure, "failure");
        LOGGER.atError()
            .setCause(actual)
            .addKeyValue("event", "zlink.runtime_error")
            .addKeyValue("kind", "message_flow_observer_failed")
            .addKeyValue("source", sourceName)
            .addKeyValue("callback", callbackName)
            .addKeyValue("exception_type", actual.getClass().getName())
            .log("ZLink runtime callback failed");
    }

    public void publishHostStatus(ZLinkFrameworkRuntimeStatus status) {
        publishHostStatus(status, null, null);
    }

    public void publishHostStatus(
        ZLinkFrameworkRuntimeStatus status, String stage, Throwable failure) {
        Objects.requireNonNull(status, "status");
        var event = LOGGER.atInfo()
            .addKeyValue("event", "zlink.runtime_status_changed")
            .addKeyValue("state", status.state())
            .addKeyValue("sequence", status.sequence());
        if (failure != null) {
            String message = String.valueOf(failure.getMessage())
                .replace("\r", "\\r").replace("\n", "\\n");
            event.addKeyValue("stage", stage)
                .addKeyValue("exception_type", failure.getClass().getName())
                .addKeyValue("exception_message", message);
            Throwable cause = failure;
            while (cause.getCause() != null) {
                cause = cause.getCause();
            }
            String causeMessage = String.valueOf(cause.getMessage())
                .replace("\r", "\\r").replace("\n", "\\n");
            event.addKeyValue("cause_type", cause.getClass().getName())
                .addKeyValue("cause_message", causeMessage);
            var terminal = status.terminationResult().orElseThrow();
            event.addKeyValue("outcome", terminal.outcome())
                .addKeyValue("reason", terminal.reason());
            event.log("ZLink runtime termination outcome={} reason={} stage={} exception_type={} exception_message={} cause_type={} cause_message={}",
                terminal.outcome(), terminal.reason(), stage, failure.getClass().getName(),
                message, cause.getClass().getName(), causeMessage);
        } else {
            event.log("ZLink runtime status changed");
        }
    }
}
