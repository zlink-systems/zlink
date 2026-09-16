package systems.zlink.framework.runtime.locations;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
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
    private final Object lifecycleGate = new Object();
    private ScheduledFuture<?> task;
    private CompletionStage<Void> inFlightTick =
        CompletableFuture.completedFuture(null);
    private CompletionStage<Void> termination;
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
        synchronized (lifecycleGate) {
            running = true;
            startupPollingUntilNanos = System.nanoTime()
                + options.ownerLeaseRenewInterval().toNanos();
            inFlightTick = tick().whenComplete((ignored, failure) ->
                scheduleNext());
            return inFlightTick;
        }
    }

    CompletionStage<Void> stop() {
        CompletableFuture<Void> stopping = new CompletableFuture<>();
        CompletionStage<Void> settling;
        synchronized (lifecycleGate) {
            if (termination != null) {
                return termination;
            }
            termination = stopping;
            running = false;
            if (task != null) {
                task.cancel(false);
                task = null;
            }
            executor.shutdownNow();
            settling = inFlightTick;
        }
        settling.whenComplete((ignored, tickFailure) ->
            reconciler.shutdown().whenComplete((shutdownIgnored, shutdownFailure) -> {
                Throwable failure = tickFailure == null
                    ? shutdownFailure : tickFailure;
                if (tickFailure != null && shutdownFailure != null) {
                    tickFailure.addSuppressed(shutdownFailure);
                }
                if (failure == null) {
                    stopping.complete(null);
                } else {
                    stopping.completeExceptionally(failure);
                }
            }));
        return stopping;
    }

    CompletionStage<Void> markDraining() {
        return reconciler.markDraining();
    }

    private CompletionStage<Void> tick() {
        return reconciler.tick();
    }

    private void tickOnLoop() {
        synchronized (lifecycleGate) {
            task = null;
            if (!running) {
                return;
            }
            inFlightTick = tick().whenComplete((ignored, failure) ->
                scheduleNext());
        }
    }

    private void scheduleNext() {
        synchronized (lifecycleGate) {
            if (!running) {
                return;
            }
            long delayMillis = options.pollingInterval().toMillis();
            if (System.nanoTime() < startupPollingUntilNanos) {
                delayMillis = Math.min(delayMillis, STARTUP_POLLING_MILLIS);
            }
            task = executor.schedule(
                this::tickOnLoop, delayMillis, TimeUnit.MILLISECONDS);
        }
    }

    @Override
    public void close() {
        stop();
    }
}
