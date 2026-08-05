package systems.zlink.framework.runtime.channels;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import java.util.function.BooleanSupplier;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

final class ZLinkSpotRouteBridgeDrainer {
    private static final long DRAIN_INTERVAL_MILLIS = 10;

    private final Map<String, ZLinkBackendSpotRouteBridge> bridges;
    private final ScheduledExecutorService scheduler;
    private final BooleanSupplier running;
    private final BiConsumer<String, Throwable> failureReporter;
    private volatile Runnable dispatchDrainer;
    private int nextChannelCursor;
    private volatile boolean started;

    ZLinkSpotRouteBridgeDrainer(
        Map<String, ZLinkBackendSpotRouteBridge> bridges,
        ScheduledExecutorService scheduler,
        BooleanSupplier running,
        BiConsumer<String, Throwable> failureReporter) {
        this.bridges = bridges;
        this.scheduler = scheduler;
        this.running = running;
        this.failureReporter = failureReporter;
    }

    void setDispatchDrainer(Runnable dispatchDrainer) {
        this.dispatchDrainer = dispatchDrainer;
    }

    void start() {
        if (started) {
            return;
        }
        synchronized (scheduler) {
            if (started) {
                return;
            }
            started = true;
            scheduler.scheduleWithFixedDelay(
                this::drainAll,
                0,
                DRAIN_INTERVAL_MILLIS,
                TimeUnit.MILLISECONDS);
        }
    }

    void drainNow(String channelName) {
        ZLinkBackendSpotRouteBridge bridge = bridges.get(channelName);
        if (bridge == null) {
            return;
        }
        try {
            synchronized (bridge) {
                bridge.drain();
            }
        } catch (RuntimeException error) {
            if (ZLinkChannelRuntime.isNoDataReceive(error)) {
                return;
            }
            throw error;
        }
        runDispatchDrainer();
    }

    private void drainAll() {
        if (!running.getAsBoolean()) {
            return;
        }
        List<String> channelNames = bridges.keySet().stream().sorted().toList();
        if (channelNames.isEmpty()) {
            return;
        }
        int start = Math.floorMod(nextChannelCursor, channelNames.size());
        nextChannelCursor = (start + 1) % channelNames.size();
        for (int offset = 0; offset < channelNames.size(); offset++) {
            String channelName = channelNames.get((start + offset) % channelNames.size());
            try {
                drainNow(channelName);
            } catch (RuntimeException error) {
                if (!ZLinkChannelRuntime.isNoDataReceive(error) && running.getAsBoolean()) {
                    failureReporter.accept(channelName, error);
                }
            }
        }
    }

    private void runDispatchDrainer() {
        Runnable drainer = dispatchDrainer;
        if (drainer != null) {
            drainer.run();
        }
    }
}
