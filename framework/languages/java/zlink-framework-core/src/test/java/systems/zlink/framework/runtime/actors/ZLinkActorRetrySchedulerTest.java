package systems.zlink.framework.runtime.actors;

import org.junit.jupiter.api.Assertions;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Method;
import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.streams.ZLinkStreamCodec;

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
        CompletionException failure = Assertions.assertThrows(
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

        CompletionException observed = Assertions.assertThrows(
            CompletionException.class,
            () -> ZLinkActorRetryScheduler.retryRouteUntilPresent(
                    Duration.ofMillis(100),
                    () -> CompletableFuture.failedFuture(failure))
                .toCompletableFuture()
                .join());

        assertTrue(observed.getCause() == failure);
    }

    @Test
    void bindRetryExhaustionSurfacesDeadlineExceededWithLastAttemptAsCause() {
        IllegalStateException lastAttempt =
            new IllegalStateException("relay not connected");

        CompletionException observed = Assertions.assertThrows(
            CompletionException.class,
            () -> ZLinkActorRetryScheduler.bindRelayUntilAccepted(
                    Duration.ZERO,
                    () -> CompletableFuture.failedFuture(lastAttempt),
                    ignored -> false,
                    ignored -> true)
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException framework = assertInstanceOf(
            ZLinkFrameworkException.class, observed.getCause());
        assertEquals(
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, framework.kind());
        assertSame(lastAttempt, framework.getCause());
    }

    @Test
    void bindNonRetryableFailurePropagatesUnmapped() {
        IllegalStateException fatal = new IllegalStateException("fatal bind");

        CompletionException observed = Assertions.assertThrows(
            CompletionException.class,
            () -> ZLinkActorRetryScheduler.bindRelayUntilAccepted(
                    Duration.ofSeconds(5),
                    () -> CompletableFuture.failedFuture(fatal),
                    ignored -> false,
                    ignored -> false)
                .toCompletableFuture()
                .join());

        assertSame(fatal, observed.getCause());
    }

    @Test
    void boundSessionRouteNeverReadySurfacesDeadlineExceeded() throws Exception {
        ZLinkBoundSessionRuntime runtime = new ZLinkBoundSessionRuntime(
            null,
            null,
            RoutingId.from("session-a"),
            "actor-1",
            null,
            null,
            null,
            ZLinkStreamCodec.JSON,
            ignored -> false,
            ZLinkRelayMetadataPolicy.EMPTY);
        Method awaitRouteReady = ZLinkBoundSessionRuntime.class
            .getDeclaredMethod(
                "awaitRouteReady",
                ZLinkBackendActorRef.class,
                Duration.class);
        awaitRouteReady.setAccessible(true);

        @SuppressWarnings("unchecked")
        CompletionStage<Void> stage = (CompletionStage<Void>)
            awaitRouteReady.invoke(
                runtime,
                new ZLinkBackendActorRef(
                    RoutingId.from("actor-node-a"), "actor-1", 1),
                Duration.ZERO);

        CompletionException observed = Assertions.assertThrows(
            CompletionException.class,
            () -> stage.toCompletableFuture().join());

        ZLinkFrameworkException framework = assertInstanceOf(
            ZLinkFrameworkException.class, observed.getCause());
        assertEquals(
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, framework.kind());
        assertInstanceOf(TimeoutException.class, framework.getCause());
    }

}
