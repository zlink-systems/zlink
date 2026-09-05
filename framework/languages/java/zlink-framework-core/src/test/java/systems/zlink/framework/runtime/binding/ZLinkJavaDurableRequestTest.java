package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

final class ZLinkJavaDurableRequestTest {
    enum Operation { ACTOR_JOIN, ACTOR_CREATE, BOUND_SESSION_BIND }

    @Test
    void logicalTargetRemovalEndsAdmittedReplayAsUnavailable() throws Exception {
        var ended = new AtomicBoolean();
        var disconnected = new ZlinkRequestException(RequestResult.NOT_CONNECTED);
        var submitted = new CompletableFuture<List<byte[]>>();
        var attempts = new AtomicInteger();
        var completion = ZLinkJavaDurableRequest.request(
            () -> List.of(new byte[] {1}), (frames, remaining) -> {
                attempts.incrementAndGet();
                return submitted;
            }, ended::get, Duration.ofSeconds(5)).toCompletableFuture();

        ended.set(true);
        submitted.completeExceptionally(disconnected);

        var failure = assertThrows(java.util.concurrent.ExecutionException.class,
            () -> completion.get(1, TimeUnit.SECONDS));
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        assertEquals(1, attempts.get(), "removed target must not be resubmitted");
    }

    @Test
    void removedTargetIsNotSubmittedEvenBeforeFirstAdmission() {
        var attempts = new AtomicInteger();
        var completion = ZLinkJavaDurableRequest.request(
            () -> List.of(new byte[] {1}), (frames, remaining) -> {
                attempts.incrementAndGet();
                return CompletableFuture.completedFuture(List.of());
            }, () -> true, Duration.ofSeconds(5));
        assertFailure(completion.toCompletableFuture(),
            ZLinkFrameworkErrorKind.UNAVAILABLE, null);
        assertEquals(0, attempts.get());
    }

    @ParameterizedTest
    @EnumSource(Operation.class)
    void neverAdmittedExhaustsAsUnavailable(Operation operation) throws Exception {
        var failure = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
        AtomicInteger attempts = new AtomicInteger();
        List<byte[]> header = encode(operation);
        var completion = ZLinkJavaDurableRequest.request(() -> header,
            (frames, remaining) -> {
                assertSame(header, frames);
                attempts.incrementAndGet();
                return CompletableFuture.failedFuture(failure);
            }, () -> false, Duration.ofMillis(80));
        assertFailure(completion.toCompletableFuture(),
            ZLinkFrameworkErrorKind.UNAVAILABLE, failure);
        assertTrue(attempts.get() > 1);
    }

    @ParameterizedTest
    @EnumSource(Operation.class)
    void admittedLostReplyExhaustsAsDeadlineExceeded(Operation operation) throws Exception {
        var failure = new ZlinkRequestException(RequestResult.TIMED_OUT);
        AtomicInteger attempts = new AtomicInteger();
        List<byte[]> header = encode(operation);
        var completion = ZLinkJavaDurableRequest.request(() -> header,
            (frames, remaining) -> {
                attempts.incrementAndGet();
                var pending = new CompletableFuture<List<byte[]>>();
                CompletableFuture.delayedExecutor(remaining.toNanos(), TimeUnit.NANOSECONDS)
                    .execute(() -> pending.completeExceptionally(failure));
                return pending;
            }, () -> false, Duration.ofMillis(80));
        assertFailure(completion.toCompletableFuture(),
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, failure);
        assertEquals(1, attempts.get(), "the attempt owns the whole remaining deadline");
    }

    @ParameterizedTest
    @EnumSource(Operation.class)
    void handoverReplaysIdenticalHeaderWithWholeRemainingDeadline(Operation operation)
        throws Exception {
        AtomicInteger preparations = new AtomicInteger();
        AtomicInteger attempts = new AtomicInteger();
        AtomicInteger executions = new AtomicInteger();
        List<byte[]> header = encode(operation);
        byte[] snapshot = header.getFirst().clone();
        List<byte[]> terminal = List.of(new byte[] {7});
        long started = System.nanoTime();
        Duration timeout = Duration.ofSeconds(3);
        var completion = ZLinkJavaDurableRequest.request(() -> {
                preparations.incrementAndGet();
                return header;
            }, (frames, remaining) -> {
                assertSame(header, frames);
                assertArrayEquals(snapshot, frames.getFirst());
                long elapsed = System.nanoTime() - started;
                assertTrue(remaining.toNanos() >= timeout.toNanos() - elapsed);
                assertTrue(remaining.compareTo(timeout) <= 0);
                if (attempts.incrementAndGet() == 1) {
                    // The target already executed; Core completed the request
                    // stranded by handover before its reply was delivered.
                    executions.incrementAndGet();
                    return CompletableFuture.failedFuture(
                        new ZlinkRequestException(RequestResult.NOT_CONNECTED));
                }
                return CompletableFuture.completedFuture(terminal);
            }, () -> false, timeout);
        assertSame(terminal, completion.toCompletableFuture().get(1, TimeUnit.SECONDS));
        assertEquals(1, preparations.get());
        assertEquals(2, attempts.get());
        assertEquals(1, executions.get(), "replay retrieves the target terminal record");
    }

    @Test
    void preflightDoesNotInventAnAdmittedRequest() {
        AtomicInteger submits = new AtomicInteger();
        var completion = ZLinkJavaDurableRequest.request(() -> null,
            (frames, remaining) -> {
                submits.incrementAndGet();
                return CompletableFuture.completedFuture(List.of());
            }, () -> false, Duration.ofMillis(30));
        assertFailure(completion.toCompletableFuture(),
            ZLinkFrameworkErrorKind.UNAVAILABLE, null);
        assertEquals(0, submits.get());
    }

    @Test
    void admissionHistorySurvivesLaterSubmitFailures() throws Exception {
        AtomicInteger attempts = new AtomicInteger();
        var last = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
        List<byte[]> header = encode(Operation.ACTOR_CREATE);
        var completion = ZLinkJavaDurableRequest.request(() -> header,
            (frames, remaining) -> CompletableFuture.failedFuture(
                attempts.incrementAndGet() == 1
                    ? new ZlinkRequestException(RequestResult.NOT_CONNECTED) : last),
            () -> false, Duration.ofMillis(80));
        assertFailure(completion.toCompletableFuture(),
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, last);
        assertTrue(attempts.get() > 1);
    }

    @Test
    void synchronousSubmitFailureRetainsItsTypedCause() {
        var failure = new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
        var completion = ZLinkJavaDurableRequest.request(() -> List.of(new byte[] {1}),
            (frames, remaining) -> { throw failure; }, () -> false, Duration.ofMillis(30));
        assertFailure(completion.toCompletableFuture(),
            ZLinkFrameworkErrorKind.UNAVAILABLE, failure);
    }

    @Test
    void terminalReplyAndDecodeFailureDoNotReplay() {
        AtomicInteger attempts = new AtomicInteger();
        var malformed = new IllegalArgumentException("malformed terminal");
        var completion = ZLinkJavaDurableRequest.request(() -> List.of(new byte[] {1}),
            (frames, remaining) -> {
                attempts.incrementAndGet();
                return CompletableFuture.completedFuture(List.of(new byte[] {0}));
            }, () -> false, Duration.ofSeconds(1))
            .thenApply(frames -> { throw malformed; });
        assertSame(malformed, assertThrows(CompletionException.class,
            () -> completion.toCompletableFuture().join()).getCause());
        assertEquals(1, attempts.get());
    }

    @Test
    void permanentBindingFailureIsPreserved() {
        var failure = new ZlinkSubmitException(SubmitResult.TERMINATED);
        var completion = ZLinkJavaDurableRequest.request(() -> List.of(new byte[] {1}),
            (frames, remaining) -> CompletableFuture.failedFuture(failure),
            () -> false, Duration.ofSeconds(1));
        assertSame(failure, assertThrows(CompletionException.class,
            () -> completion.toCompletableFuture().join()).getCause());
    }

    private static void assertFailure(CompletableFuture<?> completion,
        ZLinkFrameworkErrorKind kind, Throwable cause) {
        var failure = assertThrows(CompletionException.class, completion::join);
        var framework = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
        assertEquals(kind, framework.kind());
        assertSame(cause, framework.getCause());
    }

    private static List<byte[]> encode(Operation operation) throws java.io.IOException {
        RoutingId source = RoutingId.from("source");
        RoutingId target = RoutingId.from("target");
        var codec = new ZLinkServiceM6BWireCodec();
        return switch (operation) {
            case ACTOR_JOIN -> ServiceWirePilotCodec.encodeActorJoin28(
                new ServiceWirePilotCodec.ActorJoin28(17,
                    new ServiceWirePilotCodec.Fence("actor", 3, source.toBytes(), 4, 5, 6),
                    false,
                    new ServiceWirePilotCodec.Fence("spot", 7, target.toBytes(), 8, 9, 10),
                    new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
                        "join", "application/json", new byte[] {123, 125})));
            case ACTOR_CREATE -> List.of(codec.encodeActorCreateHeader(
                new ZLinkServiceM6BWireCodec.ActorCreate(17, 18, 19, source, 4,
                    "actor", "player", new ZLinkServiceM6BWireCodec.ReservationFence(
                        "reservation", "version", 9, 10, target, 8, "owner", 6, 1),
                    1_900_000_000_000L)));
            case BOUND_SESSION_BIND -> List.of(codec.encodeBoundSessionBindHeader(
                new ZLinkServiceM6BWireCodec.BoundSessionBind(17,
                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                        new ZLinkBackendActorRef(target, "actor", 3), 8, 9, 10),
                    RoutingId.from("session"), true, 11)));
        };
    }
}
