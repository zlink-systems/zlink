package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Modifier;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

@Timeout(10)
final class ZLinkChannelBlockingSubmissionTest {
    @Test
    void blockingSignaturesMatchTheExactJavaInterfaces() throws Exception {
        var send = ZLinkSendCall.class.getMethod("submit_sync");
        var request = ZLinkRequestCall.class.getMethod("submit_sync", Class.class);
        assertEquals(void.class, send.getReturnType());
        assertTrue(Modifier.isAbstract(send.getModifiers()));
        assertEquals("TReply", request.getGenericReturnType().getTypeName());
        assertTrue(Modifier.isAbstract(request.getModifiers()));
    }

    @Test
    void applicationThreadReceivesAdmissionAndTypedReply() {
        try (Fixture f = new Fixture()) {
            f.complete();
            f.send.submit_sync();
            assertEquals(new Reply("accepted"), f.request.submit_sync(Reply.class));
            assertEquals(2, f.submissions.get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void applicationThreadBlocksUntilAdmissionOrReply(boolean request) throws Exception {
        try (Fixture f = new Fixture(); var callers = Executors.newVirtualThreadPerTaskExecutor()) {
            var result = callers.submit(() -> {
                if (request) {
                    return f.request.submit_sync(Reply.class);
                }
                f.send.submit_sync();
                return null;
            });
            try {
                assertTrue(f.submitted.await(5, TimeUnit.SECONDS));
                assertThrows(TimeoutException.class, () -> result.get(100, TimeUnit.MILLISECONDS),
                    "submission alone must not complete the blocking terminal");
                f.complete();
                assertEquals(request ? new Reply("accepted") : null, result.get(5, TimeUnit.SECONDS));
                assertEquals(1, f.submissions.get());
            } finally {
                f.complete();
            }
        }
    }

    @Test
    void handlerTurnRejectsBothTerminalsBeforeSubmissionAndLeavesCallsUsable() {
        try (Fixture f = new Fixture(); var jobs = new ZLinkApplicationJobQueue(
                 ZLinkApplicationJobQueueProfile.BALANCED, OptionalLong.of(1),
                 new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            f.complete();
            var serial = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.generic());
            var permit = jobs.acquire().toCompletableFuture().join();
            CompletionStage<Void> turn;
            try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                turn = serial.enqueue(() -> ZLinkHandlerMethodInvoker.invokeHandler(
                    new BlockingHandler(() -> {
                        assertTrue(ZLinkApplicationJobContext.current().isPresent());
                        assertNull(ZLinkSuspendInvocationContext.currentApplicationExecution());
                        assertNull(ZLinkStateLane.current());
                        f.assertRejected();
                    }), "handle", new Object[0], List.of()).thenApply(ignoredReply -> null));
            } finally {
                permit.abandonReservation();
            }
            turn.toCompletableFuture().join();
            serial.awaitQuiescence().toCompletableFuture().join();
            f.assertCallsStillUsable();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void spotTurnRejectsBothTerminalsWithOnlyTheExistingSpotMarker(boolean sharedGate) {
        try (Fixture f = new Fixture()) {
            f.complete();
            var activation = new ZLinkSuspendInvocationContext.ApplicationExecution(
                "spot", null, sharedGate, sharedGate, ignored -> false);
            try (var ignored = ZLinkSuspendInvocationContext.enterApplicationExecution(activation)) {
                assertTrue(ZLinkApplicationJobContext.current().isEmpty());
                assertNull(ZLinkStateLane.current());
                f.assertRejected();
            }
            f.assertCallsStillUsable();
        }
    }

    @Test
    void stateLaneRejectsBothTerminalsBeforeSubmissionAndLeavesCallsUsable() {
        var lane = new ZLinkStateLane(Runnable::run);
        try (Fixture f = new Fixture()) {
            f.complete();
            lane.runAsync(() -> {
                assertTrue(ZLinkApplicationJobContext.current().isEmpty());
                assertNull(ZLinkSuspendInvocationContext.currentApplicationExecution());
                assertSame(lane, ZLinkStateLane.current());
                f.assertRejected();
            }).toCompletableFuture().join();
            f.assertCallsStillUsable();
        } finally {
            lane.closeAsync().toCompletableFuture().join();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void syncAndAsyncTerminalsShareTheSingleUseGate(boolean syncFirst) {
        try (Fixture f = new Fixture()) {
            f.complete();
            if (syncFirst) {
                f.send.submit_sync();
                f.request.submit_sync(Reply.class);
                assertInvalid(assertThrows(CompletionException.class,
                    () -> f.send.submit().toCompletableFuture().join()).getCause());
                assertInvalid(assertThrows(CompletionException.class,
                    () -> f.request.submit(Reply.class).toCompletableFuture().join()).getCause());
            } else {
                f.send.submit().toCompletableFuture().join();
                f.request.submit(Reply.class).toCompletableFuture().join();
                assertInvalid(assertThrows(ZLinkFrameworkException.class, f.send::submit_sync));
                assertInvalid(assertThrows(ZLinkFrameworkException.class,
                    () -> f.request.submit_sync(Reply.class)));
            }
            assertEquals(2, f.submissions.get());
        }
    }

    @Test
    void blockingTerminalsPreserveFrameworkFailureKindsWithoutCompletionWrappers() {
        try (Fixture f = new Fixture()) {
            var failure = new ZLinkFrameworkException(ZLinkFrameworkErrorKind.UNAVAILABLE, "route lost");
            f.admission.completeExceptionally(failure);
            f.reply.completeExceptionally(failure);
            assertSame(failure, assertThrows(ZLinkFrameworkException.class, f.send::submit_sync));
            assertSame(failure, assertThrows(ZLinkFrameworkException.class,
                () -> f.request.submit_sync(Reply.class)));
            assertEquals(2, f.submissions.get());
        }
    }

    private static void assertInvalid(Throwable failure) {
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION,
            assertInstanceOf(ZLinkFrameworkException.class, failure).kind());
    }

    public static final class BlockingHandler {
        private final Runnable check;

        BlockingHandler(Runnable check) {
            this.check = check;
        }

        public CompletionStage<Void> handle() {
            check.run();
            return CompletableFuture.completedFuture(null);
        }
    }

    private record Reply(String value) { }

    private static final class Fixture implements AutoCloseable {
        final ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
        final CompletableFuture<Void> admission = new CompletableFuture<>();
        final CompletableFuture<ZLinkBackendReceived> reply = new CompletableFuture<>();
        final AtomicInteger submissions = new AtomicInteger();
        final CountDownLatch submitted = new CountDownLatch(1);
        final Message sendPayload = Message.from("send");
        final Message requestPayload = Message.from("request");
        final ZLinkChannelCallRuntime runtime;
        final ZLinkSendCall send;
        final ZLinkRequestCall request;

        Fixture() {
            var options = new DefaultZLinkFrameworkOptions();
            runtime = new ZLinkChannelCallRuntime(new ZLinkMessageFlowTracer(
                options.registration().dispatchOptions(), null, Runnable::run), scheduler,
                new ZLinkChannelReplyDecoder(new ZLinkJsonMessageSerializer()), null, null);
            var node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(getClass().getClassLoader(),
                new Class<?>[] {ZLinkInternalSpotNode.class}, (proxy, method, args) -> {
                    if (method.getName().equals("sendToChannel")) {
                        submissions.incrementAndGet();
                        submitted.countDown();
                        return admission;
                    }
                    if (method.getName().equals("requestToChannel")) {
                        var operations = (ZLinkServiceOperationRegistry) args[4];
                        return operations.submit((UUID) args[5], (Duration) args[3], () -> {
                            submissions.incrementAndGet();
                            submitted.countDown();
                            return reply;
                        }, ZLinkBackendReceived::close);
                    }
                    throw new AssertionError("unexpected backend call: " + method);
                });
            sockets.registerSpotRouterNode("orders", node);
            send = new ChannelSendCall(runtime, "orders", sockets, Duration.ofSeconds(5),
                sendPayload, Optional.of("packet"));
            request = new ChannelRequestCall(runtime, "orders", sockets, Duration.ofSeconds(5),
                requestPayload, Optional.of("packet"), null);
        }

        void complete() {
            admission.complete(null);
            if (!reply.isDone()) {
                reply.complete(new ZLinkBackendReceived(ZLinkBackendRequestResult.OK,
                    Optional.empty(), Optional.empty(), Optional.empty(),
                    List.of(Message.from(new ZLinkJsonMessageSerializer().serialize(new Reply("accepted")).bytes()))));
            }
        }

        void assertRejected() {
            assertInvalid(assertThrows(ZLinkFrameworkException.class, send::submit_sync));
            assertInvalid(assertThrows(ZLinkFrameworkException.class, () -> request.submit_sync(Reply.class)));
            assertEquals(0, submissions.get(), "runtime rejection must precede backend submission");
            assertFalse(sendPayload.empty(), "runtime rejection must preserve the send payload");
            assertFalse(requestPayload.empty(), "runtime rejection must preserve the request payload");
        }

        void assertCallsStillUsable() {
            assertTrue(ZLinkApplicationJobContext.current().isEmpty());
            assertNull(ZLinkSuspendInvocationContext.currentApplicationExecution());
            assertNull(ZLinkStateLane.current());
            send.submit_sync();
            assertEquals(new Reply("accepted"), request.submit_sync(Reply.class));
            assertEquals(2, submissions.get(), "runtime rejection must not consume either terminal gate");
        }

        @Override
        public void close() {
            runtime.beginClose();
            scheduler.shutdownNow();
            if (reply.isDone() && !reply.isCompletedExceptionally()) {
                reply.join().close();
            }
            sendPayload.close();
            requestPayload.close();
        }
    }
}
