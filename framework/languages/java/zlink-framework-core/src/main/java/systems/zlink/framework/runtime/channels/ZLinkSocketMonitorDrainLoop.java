package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

import java.util.function.Consumer;

final class ZLinkSocketMonitorDrainLoop {
    private ZLinkSocketMonitorDrainLoop() {}

    static Thread start(
            String threadName,
            ZLinkBackendSocketMonitor monitor,
            Consumer<ZLinkBackendSocketMonitorEvent> dispatch) {
        return Thread.ofVirtual()
                .name(threadName)
                .start(
                        () -> {
                            Thread current = Thread.currentThread();
                            while (!current.isInterrupted() && !monitor.isClosed()) {
                                if (!monitor.waitForReadable(
                                        ZLinkBackendSocketMonitor.WAIT_UNTIL_EVENT)) {
                                    continue;
                                }
                                ZLinkBackendSocketMonitorEvent event;
                                while (!current.isInterrupted()
                                        && (event = monitor.recvDontWait()) != null) {
                                    dispatch.accept(event);
                                }
                            }
                        });
    }
}
