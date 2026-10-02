package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;

public interface ZLinkBackendSocketMonitor extends ZLinkBackendObject {
    Duration WAIT_UNTIL_EVENT = Duration.ofMillis(-1);

    boolean waitForReadable(Duration timeout);

    ZLinkBackendSocketMonitorEvent recvDontWait();

    boolean isClosed();
}
