package systems.zlink.framework.runtime.locations;

import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.locations.ZLinkLocationOptions;

final class ZLinkAutoConnectLoop implements AutoCloseable {
    private static final long STARTUP_POLLING_MILLIS = 100;

    private final ZLinkAutoConnectReconciler reconciler;
    private final ZLinkLocationOptions options;
    private final ScheduledExecutorService executor;
    private ScheduledFuture<?> task;
    private volatile boolean running;
    private volatile long startupPollingUntilNanos;

    ZLinkAutoConnectLoop(
        ZLinkAutoConnectReconciler reconciler,
        ZLinkLocationOptions options) {
        this.reconciler = Objects.requireNonNull(reconciler, "reconciler");
        this.options = Objects.requireNonNull(options, "options");
        this.executor = Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "zlink-location-auto-connect");
            thread.setDaemon(true);
            return thread;
        });
    }

    CompletionStage<Void> start() {
        running = true;
        startupPollingUntilNanos = System.nanoTime()
            + options.ownerLeaseRenewInterval().toNanos();
        return tick().whenComplete((ignored, failure) -> {
            if (running) {
                scheduleNext();
            }
        });
    }

    CompletionStage<Void> stop() {
        running = false;
        if (task != null) {
            task.cancel(false);
            task = null;
        }
        executor.shutdownNow();
        return reconciler.shutdown();
    }

    CompletionStage<Void> markDraining() {
        return reconciler.markDraining();
    }

    private CompletionStage<Void> tick() {
        return reconciler.tick();
    }

    private void tickOnLoop() {
        if (!running) {
            return;
        }
        tick().whenComplete((ignored, failure) -> {
            // The reconciler records store failures as fail-static ticks.
            if (running) {
                scheduleNext();
            }
        });
    }

    private void scheduleNext() {
        long delayMillis = options.pollingInterval().toMillis();
        if (System.nanoTime() < startupPollingUntilNanos) {
            delayMillis = Math.min(delayMillis, STARTUP_POLLING_MILLIS);
        }
        task = executor.schedule(this::tickOnLoop, delayMillis, TimeUnit.MILLISECONDS);
    }

    @Override
    public void close() {
        stop();
    }
}
