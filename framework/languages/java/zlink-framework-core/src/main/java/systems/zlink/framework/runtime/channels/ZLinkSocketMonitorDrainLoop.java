package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

import java.time.Duration;
import java.util.function.Consumer;

final class ZLinkSocketMonitorDrainLoop {
    private static final Duration RECEIVE_POLL_TIMEOUT = Duration.ofMillis(250);

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
                                if (!monitor.waitForReadable(RECEIVE_POLL_TIMEOUT)) {
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
