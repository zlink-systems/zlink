package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.locations.ZLinkLocationOptions;

import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

final class ZLinkAutoConnectLoop implements AutoCloseable {
    private final ZLinkAutoConnectReconciler reconciler;
    private final ZLinkLocationOptions options;
    final Supplier<CompletionStage<Void>> refreshCapacity;
    private final ScheduledExecutorService executor;
    private final Object lifecycleGate = new Object();
    private ScheduledFuture<?> task;
    private CompletableFuture<Void> inFlightTick = CompletableFuture.completedFuture(null);
    private CompletionStage<Void> termination;
    private boolean running;

    ZLinkAutoConnectLoop(ZLinkAutoConnectReconciler reconciler, ZLinkLocationOptions options) {
        this(reconciler, options, () -> CompletableFuture.completedFuture(null));
    }

    ZLinkAutoConnectLoop(
            ZLinkAutoConnectReconciler reconciler,
            ZLinkLocationOptions options,
            Supplier<CompletionStage<Void>> refreshCapacity) {
        this.refreshCapacity = Objects.requireNonNull(refreshCapacity, "refreshCapacity");
        this.reconciler = Objects.requireNonNull(reconciler, "reconciler");
        this.options = Objects.requireNonNull(options, "options");
        this.executor =
                Executors.newSingleThreadScheduledExecutor(
                        task -> {
                            Thread thread = new Thread(task, "zlink-location-auto-connect");
                            thread.setDaemon(true);
                            return thread;
                        });
    }

    long scheduledDelayNanosForTest() {
        synchronized (lifecycleGate) {
            return task.getDelay(TimeUnit.NANOSECONDS);
        }
    }

    CompletionStage<Void> start() {
        synchronized (lifecycleGate) {
            running = true;
            inFlightTick =
                    tick().toCompletableFuture().whenComplete((ignored, failure) -> scheduleNext());
            return inFlightTick;
        }
    }

    CompletionStage<Void> stop() {
        CompletableFuture<Void> stopping = new CompletableFuture<>();
        CompletableFuture<Void> settling;
        synchronized (lifecycleGate) {
            if (termination != null) {
                return termination;
            }
            termination =
                    stopping.orTimeout(
                            options.ownerLeaseRenewTimeout().toNanos(), TimeUnit.NANOSECONDS);
            running = false;
            if (task != null) {
                task.cancel(false);
                task = null;
            }
            executor.shutdownNow();
            settling = inFlightTick;
            settling.cancel(false);
        }
        settling.whenComplete(
                (ignored, tickResult) -> {
                    Throwable tickFailure =
                            tickResult instanceof CancellationException ? null : tickResult;
                    reconciler
                            .shutdown()
                            .whenComplete(
                                    (shutdownIgnored, shutdownFailure) -> {
                                        Throwable failure =
                                                tickFailure == null ? shutdownFailure : tickFailure;
                                        if (tickFailure != null && shutdownFailure != null) {
                                            tickFailure.addSuppressed(shutdownFailure);
                                        }
                                        if (failure == null) {
                                            stopping.complete(null);
                                        } else {
                                            stopping.completeExceptionally(failure);
                                        }
                                    });
                });
        return stopping;
    }

    /**
     * Applies the ordinary reconcile now instead of at the next period (MeshNode §7.1). A tick in
     * flight may have read the store before the request, so the immediate tick follows it.
     */
    void reconcileNow() {
        // Queue requests so a completed tick cannot recurse before its next task is assigned.
        runWhileRunning(
                () ->
                        executor.execute(
                                () ->
                                        runWhileRunning(
                                                () -> {
                                                    if (task == null) {
                                                        inFlightTick.whenComplete(
                                                                (ignored, failure) ->
                                                                        reconcileNow());
                                                    } else if (task.cancel(false)) {
                                                        task =
                                                                executor.schedule(
                                                                        this::tickOnLoop,
                                                                        0,
                                                                        TimeUnit.NANOSECONDS);
                                                    }
                                                })));
    }

    CompletionStage<Void> markDraining() {
        return reconciler.markDraining();
    }

    CompletionStage<Void> tick() {
        return reconciler.tick(this::runContinuation).thenCompose(ignored -> refreshCapacity.get());
    }

    private void runContinuation(Runnable continuation) {
        if (!runWhileRunning(continuation)) {
            throw new CancellationException("auto-connect loop stopped");
        }
    }

    private boolean runWhileRunning(Runnable continuation) {
        synchronized (lifecycleGate) {
            if (!running) return false;
            continuation.run();
            return true;
        }
    }

    private void tickOnLoop() {
        runWhileRunning(
                () -> {
                    task = null;
                    inFlightTick =
                            tick().toCompletableFuture()
                                    .whenComplete((ignored, failure) -> scheduleNext());
                });
    }

    private void scheduleNext() {
        runWhileRunning(
                () -> {
                    task =
                            executor.schedule(
                                    this::tickOnLoop,
                                    options.pollingInterval().toNanos(),
                                    TimeUnit.NANOSECONDS);
                });
    }

    @Override
    public void close() {
        stop();
    }
}
