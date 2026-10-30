package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.sockets.ConnectionAttempt;
import systems.zlink.contracts.sockets.ConnectionAttemptState;
import systems.zlink.contracts.sockets.ConnectionFailure;
import systems.zlink.contracts.sockets.ConnectionFailureKind;
import systems.zlink.contracts.sockets.ConnectionFailureStage;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerRouteObservation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Submit §5: connection history is checked before a waiting request can start binding. */
final class ZLinkJavaConnectionAdmissionTest {
    @Test
    void readyRequestDoesNotObserveConnectionsOrEnterTheTransportLane() throws Exception {
        try (var source = new Source()) {
            source.ready = true;
            assertEquals("reply", source.request().get(1, TimeUnit.SECONDS));
            assertEquals(1, source.submissions.get());
            assertEquals(0, source.observations.get());
            assertEquals(0, source.ownerTurns.get());
            assertEquals(0, source.connectionReads.get());
        }
    }

    @Test
    void requestThatBecomesReadyBeforeItsOwnerTurnDoesNotInstallAnObserver() throws Exception {
        try (var source = new Source()) {
            source.beforeOwnerTurn = () -> source.ready = true;
            source.reply = new CompletableFuture<>();
            var result = source.request();
            assertFalse(result.isDone());
            assertEquals(1, source.submissions.get());
            assertEquals(0, source.observations.get());
            assertEquals(1, source.ownerTurns.get());
            assertEquals(0, source.connectionReads.get());
            source.reply.complete("reply");
            assertEquals("reply", result.get(1, TimeUnit.SECONDS));
        }
    }

    @Test
    void lateAdmissionStartsBindingOnceWithTheOriginalBudget() throws Exception {
        try (var source = new Source()) {
            var result = source.request();
            assertEquals(0, source.submissions.get());
            assertFalse(result.isDone());
            assertEquals(1, source.observations.get());
            source.ready = true;
            source.signal();
            source.signal();
            assertEquals("reply", result.get(1, TimeUnit.SECONDS));
            assertEquals(1, source.submissions.get());
            assertTrue(source.remaining.compareTo(Source.TIMEOUT) <= 0);
        }
    }

    @Test
    void attemptFailedEndsBeforeDeadlineWithoutSubmitting() throws Exception {
        try (var source = new Source()) {
            var result = source.request();
            source.attempts = List.of(attempt(1, 3, ConnectionAttemptState.ATTEMPT_FAILED));
            source.signal();
            assertUnavailable(result);
            assertEquals(0, source.submissions.get());
        }
    }

    @Test
    void alreadyFailedEndpointWithoutSelectedRouteIsImmediatelyUnavailable() throws Exception {
        try (var source = new Source()) {
            source.attempts = List.of(attempt(1, 3, ConnectionAttemptState.ATTEMPT_FAILED));
            assertUnavailable(source.request());
            assertEquals(0, source.submissions.get());
        }
    }

    @Test
    void failureFollowedByRecoveryStillFailsTheWaitingRequest() throws Exception {
        try (var source = new Source()) {
            var result = source.request();
            source.attempts = List.of(attempt(1, 3, ConnectionAttemptState.CONNECTED));
            source.ready = true;
            source.signal();
            assertUnavailable(result);
            assertEquals(0, source.submissions.get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void removedOrReplacedIntentFailsEvenWhenRouteHasRecovered(boolean replace) throws Exception {
        try (var source = new Source()) {
            var result = source.request();
            source.attempts =
                    replace ? List.of(attempt(2, 0, ConnectionAttemptState.CONNECTED)) : List.of();
            source.ready = true;
            source.signal();
            assertUnavailable(result);
            assertEquals(0, source.submissions.get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void absentFirstRowUsesItsFailureHistory(boolean failed) throws Exception {
        try (var source = new Source()) {
            source.attempts = List.of();
            var result = source.request();
            assertFalse(result.isDone());
            source.attempts =
                    List.of(attempt(-1, failed ? -3 : 0, ConnectionAttemptState.CONNECTED));
            source.ready = true;
            source.signal();
            if (failed) {
                assertUnavailable(result);
                assertEquals(0, source.submissions.get());
            } else {
                assertEquals("reply", result.get(1, TimeUnit.SECONDS));
                assertEquals(1, source.submissions.get());
            }
        }
    }

    @Test
    void newRequestOnAdmittedRouteIgnoresPastOutgoingFailures() throws Exception {
        try (var source = new Source()) {
            source.attempts = List.of(attempt(1, 3, ConnectionAttemptState.ATTEMPT_FAILED));
            source.ready = true;
            assertEquals("reply", source.request().get(1, TimeUnit.SECONDS));
            assertEquals(1, source.submissions.get());
        }
    }

    @Test
    void startedBindingIsNotRestartedAfterConnectionFailure() throws Exception {
        try (var source = new Source()) {
            source.ready = true;
            source.reply = new CompletableFuture<>();
            var result = source.request();
            source.attempts = List.of(attempt(2, 3, ConnectionAttemptState.ATTEMPT_FAILED));
            source.ready = false;
            source.signal();
            source.ready = true;
            source.signal();
            assertEquals(1, source.submissions.get());
            source.reply.complete("original reply");
            assertEquals("original reply", result.get(1, TimeUnit.SECONDS));
        }
    }

    private static void assertUnavailable(CompletableFuture<String> result) throws Exception {
        // A completion timeout is a test failure; it is not accepted as an Unavailable result.
        var failure =
                assertThrows(
                        java.util.concurrent.ExecutionException.class,
                        () -> result.get(1, TimeUnit.SECONDS));
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    private static ConnectionAttempt attempt(
            long intent, long failure, ConnectionAttemptState state) {
        return new ConnectionAttempt(
                "tcp://127.0.0.1:12345",
                intent,
                1,
                failure,
                state,
                new ConnectionFailure(
                        ConnectionFailureKind.NONE, ConnectionFailureStage.NONE, 0, 0));
    }

    private static final class Source implements AutoCloseable {
        static final Duration TIMEOUT = Duration.ofSeconds(30);
        final ZLinkServiceOperationRegistry operations = new ZLinkServiceOperationRegistry();
        final AtomicInteger submissions = new AtomicInteger();
        final AtomicInteger observations = new AtomicInteger();
        final AtomicInteger ownerTurns = new AtomicInteger();
        final AtomicInteger connectionReads = new AtomicInteger();
        Runnable beforeOwnerTurn = () -> {};
        List<ConnectionAttempt> attempts =
                List.of(attempt(1, 0, ConnectionAttemptState.CONNECTING));
        boolean ready;
        Runnable signal = () -> {};
        Duration remaining;
        CompletableFuture<String> reply = CompletableFuture.completedFuture("reply");

        CompletableFuture<String> request() {
            return ZLinkJavaDurableRequest.<String>request(
                            () -> List.of(new byte[] {1}),
                            (frames, budget, admitted, terminal) -> {
                                remaining = budget;
                                submissions.incrementAndGet();
                                reply.whenComplete(terminal);
                                return reply;
                            },
                            () -> new PeerRouteObservation(ready ? 1 : 0, ready),
                            (changed, ended) -> {
                                observations.incrementAndGet();
                                signal = changed;
                                return () -> signal = () -> {};
                            },
                            action -> {
                                ownerTurns.incrementAndGet();
                                beforeOwnerTurn.run();
                                action.run();
                                return CompletableFuture.completedFuture(null);
                            },
                            TIMEOUT,
                            operations,
                            null,
                            () -> {
                                connectionReads.incrementAndGet();
                                return attempts;
                            })
                    .toCompletableFuture();
        }

        void signal() {
            signal.run();
        }

        @Override
        public void close() {
            operations.close();
        }
    }
}
