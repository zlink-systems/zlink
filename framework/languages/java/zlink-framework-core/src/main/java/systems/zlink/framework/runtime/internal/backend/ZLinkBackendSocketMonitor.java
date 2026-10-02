package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;

public interface ZLinkBackendSocketMonitor extends ZLinkBackendObject {
    Duration RECEIVE_POLL_TIMEOUT = Duration.ofMillis(250);

    boolean waitForReadable(Duration timeout);

    ZLinkBackendSocketMonitorEvent recvDontWait();

    boolean isClosed();
}
