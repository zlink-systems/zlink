package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;

/** Internal socket contract for readiness-gated Framework receives. */
public interface ZLinkBackendReceiveSocket extends ZLinkBackendObject {
    /** Waits for the socket's public receive operation to become readable. */
    boolean waitForReadable(Duration timeout);
}
