package systems.zlink.framework.configuration;

import java.util.concurrent.CompletableFuture;

// Resolve at runtime to turn message-flow tracing on/off (or change verbosity)
// without a restart. The change is read live by every dispatch surface. Thread-safe.
public interface ZLinkMessageFlowControl {
    /**
     * Synchronous compatibility bridge. Do not call from a framework execution
     * context such as a handler or callback; use {@link #setMessageFlowModeAsync}
     * there.
     */
    default void setMessageFlowMode(ZLinkMessageFlowLogMode mode) {
        setMessageFlowModeAsync(mode).join();
    }

    CompletableFuture<Void> setMessageFlowModeAsync(ZLinkMessageFlowLogMode mode);

    ZLinkMessageFlowLogMode messageFlowMode();
}
