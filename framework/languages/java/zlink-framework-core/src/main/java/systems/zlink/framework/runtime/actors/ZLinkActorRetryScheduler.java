package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;

final class ZLinkActorRetryScheduler {
    private static final Duration RELAY_RETRY_DELAY = Duration.ofMillis(10);
    private static final Duration ROUTE_RETRY_DELAY = Duration.ofMillis(20);
    private ZLinkActorRetryScheduler() {
    }

    static void execute(Runnable attempt) {
        CompletableFuture.runAsync(attempt);
    }

    static void scheduleRelay(Runnable attempt) {
        schedule(attempt, RELAY_RETRY_DELAY);
    }

    static void scheduleRoute(Runnable attempt) {
        schedule(attempt, ROUTE_RETRY_DELAY);
    }

    /**
     * Schedules the next Session route retransmission at an explicit spec
     * interval (spec 20 §5 step 8: 1s, 1s, 2s, 4s, 5s and then 5s).
     */
    static void scheduleRouteAfter(Runnable attempt, Duration delay) {
        if (delay == null || delay.isNegative() || delay.isZero()) {
            execute(attempt);
            return;
        }
        schedule(attempt, delay);
    }

    static CompletionStage<Void> delayRoute() {
        CompletableFuture<Void> delayed = new CompletableFuture<>();
        scheduleRoute(() -> delayed.complete(null));
        return delayed;
    }

    static <T> CompletionStage<T> retryRouteUntil(
        Duration timeout,
        Supplier<CompletionStage<T>> submit,
        Predicate<Throwable> retryable) {
        CompletableFuture<T> result = new CompletableFuture<>();
        long deadline = System.nanoTime() + timeout.toNanos();
        class Attempt implements Runnable {
            @Override
            public void run() {
                if (result.isDone()) {
                    return;
                }
                try {
                    submit.get().whenComplete((value, error) -> {
                        if (error == null) {
                            result.complete(value);
                            return;
                        }
                        if (!retryable.test(error) || System.nanoTime() >= deadline) {
                            result.completeExceptionally(error);
                            return;
                        }
                        scheduleRoute(this);
                    });
                } catch (RuntimeException ex) {
                    if (!retryable.test(ex) || System.nanoTime() >= deadline) {
                        result.completeExceptionally(ex);
                    } else {
                        scheduleRoute(this);
                    }
                }
            }
        }
        new Attempt().run();
        return result;
    }

    static <T> CompletionStage<Optional<T>> retryRouteUntilPresent(
        Duration timeout,
        Supplier<CompletionStage<Optional<T>>> lookup) {
        CompletableFuture<Optional<T>> result = new CompletableFuture<>();
        long deadline = System.nanoTime() + timeout.toNanos();
        class Attempt implements Runnable {
            @Override
            public void run() {
                if (result.isDone()) {
                    return;
                }
                CompletionStage<Optional<T>> lookupStage;
                try {
                    lookupStage = Objects.requireNonNull(
                        lookup.get(), "route lookup stage");
                } catch (RuntimeException failure) {
                    result.completeExceptionally(failure);
                    return;
                }
                lookupStage.whenComplete((value, error) -> {
                    if (error != null) {
                        // An empty result can represent eventual Store
                        // visibility. A Store or protocol failure is already
                        // terminal and must remain observable to the caller.
                        result.completeExceptionally(error);
                        return;
                    }
                    if (value == null || value.isPresent()) {
                        result.complete(value == null
                            ? Optional.empty()
                            : value);
                        return;
                    }
                    if (System.nanoTime() >= deadline) {
                        result.complete(Optional.empty());
                        return;
                    }
                    scheduleRoute(this);
                });
            }
        }
        new Attempt().run();
        return result;
    }

    static CompletionStage<Void> waitUntilRelay(
        Duration timeout,
        BooleanSupplier ready,
        Runnable onReady,
        Supplier<? extends Throwable> timeoutError) {
        return waitUntilRelay(timeout, ready, onReady, timeoutError, false);
    }

    static CompletionStage<Void> waitUntilRelayOrContinue(
        Duration timeout,
        BooleanSupplier ready) {
        return waitUntilRelay(timeout, ready, () -> {}, null, true);
    }

    static CompletionStage<Void> bindRelayUntilAccepted(
        Duration timeout,
        Supplier<CompletionStage<Void>> bind,
        Predicate<Throwable> acceptedFailure,
        Predicate<Throwable> retryableFailure) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        long deadline = System.nanoTime() + timeout.toNanos();
        class Attempt implements Runnable {
            @Override
            public void run() {
                if (result.isDone()) {
                    return;
                }
                try {
                    bind.get().whenComplete((ignored, error) -> {
                        if (error == null || acceptedFailure.test(error)) {
                            result.complete(null);
                            return;
                        }
                        if (!retryableFailure.test(error) || System.nanoTime() >= deadline) {
                            result.completeExceptionally(error);
                            return;
                        }
                        scheduleRelay(this);
                    });
                } catch (RuntimeException ex) {
                    result.completeExceptionally(ex);
                }
            }
        }
        new Attempt().run();
        return result;
    }

    private static CompletionStage<Void> waitUntilRelay(
        Duration timeout,
        BooleanSupplier ready,
        Runnable onReady,
        Supplier<? extends Throwable> timeoutError,
        boolean continueOnTimeout) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        long deadline = System.nanoTime() + timeout.toNanos();
        class Attempt implements Runnable {
            @Override
            public void run() {
                if (result.isDone()) {
                    return;
                }
                try {
                    if (ready.getAsBoolean()) {
                        onReady.run();
                        result.complete(null);
                        return;
                    }
                    if (System.nanoTime() >= deadline) {
                        if (continueOnTimeout) {
                            result.complete(null);
                        } else {
                            result.completeExceptionally(timeoutError.get());
                        }
                        return;
                    }
                    scheduleRelay(this);
                } catch (RuntimeException ex) {
                    result.completeExceptionally(ex);
                }
            }
        }
        new Attempt().run();
        return result;
    }

    private static void schedule(Runnable attempt, Duration delay) {
        scheduleAfter(attempt, delay);
    }

    static CompletableFuture<Void> scheduleAfter(
        Runnable attempt,
        Duration delay) {
        return CompletableFuture.runAsync(
            attempt,
            CompletableFuture.delayedExecutor(
                delay.toMillis(), TimeUnit.MILLISECONDS));
    }
}
