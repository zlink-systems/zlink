package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;

public interface ZLinkBackendSocketMonitor extends ZLinkBackendObject {
    boolean waitForReadable(Duration timeout);

    ZLinkBackendSocketMonitorEvent recvDontWait();

    boolean isClosed();
}
