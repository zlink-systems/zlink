package systems.zlink.framework.runtime.internal.backend;

import java.util.function.Consumer;

public final class ZLinkSocketMonitorDrainLoop {
    private ZLinkSocketMonitorDrainLoop() {}

    public static Thread start(String threadName, Runnable drain) {
        // Native poller waits pin virtual-thread carriers and prevent lane progress.
        return Thread.ofPlatform().daemon().name(threadName).start(drain);
    }

    public static Thread start(
            String threadName,
            ZLinkBackendSocketMonitor monitor,
            Consumer<ZLinkBackendSocketMonitorEvent> dispatch) {
        return start(
                threadName,
                () -> {
                    Thread current = Thread.currentThread();
                    while (!current.isInterrupted() && !monitor.isClosed()) {
                        if (!monitor.waitForReadable(ZLinkBackendSocketMonitor.WAIT_UNTIL_EVENT)) {
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
