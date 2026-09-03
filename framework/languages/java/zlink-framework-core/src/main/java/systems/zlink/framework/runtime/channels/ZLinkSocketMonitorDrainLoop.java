package systems.zlink.framework.runtime.channels;

import java.util.function.Consumer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

final class ZLinkSocketMonitorDrainLoop {
    private ZLinkSocketMonitorDrainLoop() {
    }

    static void start(
        String threadName,
        ZLinkBackendSocketMonitor monitor,
        Consumer<ZLinkBackendSocketMonitorEvent> dispatch) {
        Thread.ofVirtual().name(threadName).start(() -> {
            while (!Thread.currentThread().isInterrupted()) {
                try {
                    ZLinkBackendSocketMonitorEvent event = monitor.recv();
                    if (event == null) {
                        return;
                    }
                    dispatch.accept(event);
                } catch (RuntimeException closedOrFailed) {
                    return;
                }
            }
        });
    }
}
