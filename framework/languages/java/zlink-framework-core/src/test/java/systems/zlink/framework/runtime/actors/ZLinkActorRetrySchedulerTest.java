package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;

final class ZLinkActorRetrySchedulerTest {
    @Test
    void waitUntilRelayRunsReadyHookWhenConditionIsReady() {
        AtomicBoolean readyHookCalled = new AtomicBoolean();

        ZLinkActorRetryScheduler.waitUntilRelay(
                Duration.ofMillis(1),
                () -> true,
                () -> readyHookCalled.set(true),
                IllegalStateException::new)
            .toCompletableFuture()
            .join();

        assertTrue(readyHookCalled.get());
    }

    @Test
    void waitUntilRelayFailsWithTimeoutErrorWhenConditionDoesNotBecomeReady() {
        AtomicInteger attempts = new AtomicInteger();
        CompletionException failure = org.junit.jupiter.api.Assertions.assertThrows(
            CompletionException.class,
            () -> ZLinkActorRetryScheduler.waitUntilRelay(
                    Duration.ZERO,
                    () -> {
                        attempts.incrementAndGet();
                        return false;
                    },
                    () -> {},
                    IllegalStateException::new)
                .toCompletableFuture()
                .join());

        assertInstanceOf(IllegalStateException.class, failure.getCause());
        assertTrue(attempts.get() >= 1);
    }

    @Test
    void waitUntilRelayOrContinueCompletesOnTimeout() {
        ZLinkActorRetryScheduler.waitUntilRelayOrContinue(
                Duration.ZERO,
                () -> false)
            .toCompletableFuture()
            .join();
    }

    @Test
    void submitRelayUntilAcceptedRetriesFalseSubmitResult() {
        AtomicInteger attempts = new AtomicInteger();

        ZLinkActorRetryScheduler.submitRelayUntilAccepted(
                Duration.ofMillis(100),
                () -> attempts.incrementAndGet() >= 2,
                IllegalStateException::new)
            .toCompletableFuture()
            .join();

        assertTrue(attempts.get() >= 2);
    }

    @Test
    void submitRelayUntilAcceptedFailsNonRetryableSubmitException() {
        CompletionException failure = org.junit.jupiter.api.Assertions.assertThrows(
            CompletionException.class,
            () -> ZLinkActorRetryScheduler.submitRelayUntilAccepted(
                    Duration.ofMillis(100),
                    () -> {
                        throw new ZlinkSubmitException(SubmitResult.INVALID_ARGUMENT);
                    },
                    IllegalStateException::new)
                .toCompletableFuture()
                .join());

        assertInstanceOf(ZlinkSubmitException.class, failure.getCause());
    }

    @Test
    void storedRelayRetriesTransientNotConnectedResult() {
        AtomicInteger attempts = new AtomicInteger();

        ZLinkActorRetryScheduler.submitStoredRelayUntilAccepted(
                Duration.ofMillis(100),
                () -> {
                    if (attempts.incrementAndGet() == 1) {
                        throw new ZlinkSubmitException(
                            SubmitResult.NOT_CONNECTED);
                    }
                    return true;
                },
                IllegalStateException::new)
            .toCompletableFuture()
            .join();

        assertTrue(attempts.get() >= 2);
    }

    @Test
    void routeLookupRetriesUntilACommittedValueIsVisible() {
        AtomicInteger attempts = new AtomicInteger();

        Optional<String> value = ZLinkActorRetryScheduler.retryRouteUntilPresent(
                Duration.ofMillis(100),
                () -> CompletableFuture.completedFuture(
                    attempts.incrementAndGet() < 2
                        ? Optional.empty()
                        : Optional.of("committed")))
            .toCompletableFuture()
            .join();

        assertTrue(value.isPresent());
        assertTrue(attempts.get() >= 2);
    }

    @Test
    void routeLookupPreservesStoreFailure() {
        IllegalStateException failure = new IllegalStateException("store unavailable");

        CompletionException observed = org.junit.jupiter.api.Assertions.assertThrows(
            CompletionException.class,
            () -> ZLinkActorRetryScheduler.retryRouteUntilPresent(
                    Duration.ofMillis(100),
                    () -> CompletableFuture.failedFuture(failure))
                .toCompletableFuture()
                .join());

        assertTrue(observed.getCause() == failure);
    }

}
