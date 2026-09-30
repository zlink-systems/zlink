package systems.zlink.framework.perf;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;

// Setup waits on public status (§16.1): a status is polled on a timer thread until it shows the awaited state or the
// setup deadline passes. Only status queries repeat here; the public call being prepared is never retried.
public final class Polling {
    private Polling() {}

    private static final ScheduledExecutorService TIMER = Executors.newSingleThreadScheduledExecutor(runnable -> {
        Thread thread = new Thread(runnable, "perf-status-poll");
        thread.setDaemon(true);
        return thread;
    });

    /** Completes when {@code condition} is true, or with a TimeoutException after {@code timeoutMs}. */
    public static CompletableFuture<Void> until(BooleanSupplier condition, long intervalMs, long timeoutMs) {
        CompletableFuture<Void> done = new CompletableFuture<>();
        long deadline = System.nanoTime() + timeoutMs * 1_000_000L;
        poll(condition, intervalMs, deadline, done);
        return done;
    }

    /** Same as {@link #until} for a status that is itself an asynchronous public query (e.g. the Spot manager's find). */
    public static CompletableFuture<Void> untilAsync(Supplier<CompletionStage<Boolean>> condition, long intervalMs, long timeoutMs) {
        CompletableFuture<Void> done = new CompletableFuture<>();
        long deadline = System.nanoTime() + timeoutMs * 1_000_000L;
        pollAsync(condition, intervalMs, deadline, done);
        return done;
    }

    private static void pollAsync(Supplier<CompletionStage<Boolean>> condition, long intervalMs, long deadline,
            CompletableFuture<Void> done) {
        CompletionStage<Boolean> observed;
        try {
            observed = condition.get();
        } catch (RuntimeException error) {
            done.completeExceptionally(error);
            return;
        }
        observed.whenComplete((present, error) -> {
            if (error != null) {
                done.completeExceptionally(error);
            } else if (present) {
                done.complete(null);
            } else if (System.nanoTime() - deadline >= 0) {
                done.completeExceptionally(new TimeoutException("The awaited public status did not appear inside its setup bound."));
            } else {
                TIMER.schedule(() -> pollAsync(condition, intervalMs, deadline, done), intervalMs, TimeUnit.MILLISECONDS);
            }
        });
    }

    private static void poll(BooleanSupplier condition, long intervalMs, long deadline, CompletableFuture<Void> done) {
        try {
            if (condition.getAsBoolean()) {
                done.complete(null);
                return;
            }
            if (System.nanoTime() - deadline >= 0) {
                done.completeExceptionally(new TimeoutException("The awaited public status did not appear inside its setup bound."));
                return;
            }
            TIMER.schedule(() -> poll(condition, intervalMs, deadline, done), intervalMs, TimeUnit.MILLISECONDS);
        } catch (RuntimeException error) {
            done.completeExceptionally(error);
        }
    }
}
