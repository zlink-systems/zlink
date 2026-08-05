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
        Objects.requireNonNull(status, "status");
        LOGGER.atInfo()
            .addKeyValue("event", "zlink.runtime_status_changed")
            .addKeyValue("state", status.state())
            .addKeyValue("sequence", status.sequence())
            .log("ZLink runtime status changed");
    }
}
