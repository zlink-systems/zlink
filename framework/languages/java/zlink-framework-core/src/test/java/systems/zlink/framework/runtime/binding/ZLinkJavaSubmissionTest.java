package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaSubmissionTest {
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    private static final RoutingId TARGET = RoutingId.from("submission-target");

    enum Adapter {
        SOCKET,
        SERVICE
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void okSendCompletesWithoutReadingAdmissionAndPreservesMultipart(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message first = Message.from(new byte[] {1, 2});
                Message second = Message.from(new byte[] {3, 4})) {
            var completed = binding.send(adapter, List.of(first, second));

            assertTrue(completed.toCompletableFuture().isDone());
            assertNull(completed.toCompletableFuture().join());
            assertTrue(ZLinkOneWayCalls.isImmediateAdmission(completed));
            assertFalse(binding.admission.isDone());
            assertEquals(1, binding.resultReads);
            assertEquals(0, binding.admissionReads);
            assertEquals(1, binding.submissions);
            assertArrayEquals(new byte[] {1, 2}, binding.frames.get(0));
            assertArrayEquals(new byte[] {3, 4}, binding.frames.get(1));
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void cancellingAnOkSendFutureDoesNotAffectTheNextOkSend(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message firstPart = Message.from("first");
                Message nextPart = Message.from("next")) {
            var first = binding.send(adapter, List.of(firstPart));
            var callerFuture = first.toCompletableFuture();

            assertFalse(callerFuture.cancel(true), "OK admission has already completed");
            var next = binding.send(adapter, List.of(nextPart)).toCompletableFuture();

            assertTrue(next.isDone());
            assertFalse(next.isCancelled());
            assertNull(next.join());
            assertNull(first.toCompletableFuture().join());
            assertEquals(2, binding.submissions);
            assertEquals(0, binding.admissionReads);
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void callerFutureMutationsDoNotChangeLaterOkSends(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message firstPart = Message.from("first");
                Message nextSocketPart = Message.from("next-socket");
                Message nextServicePart = Message.from("next-service")) {
            var first = binding.send(adapter, List.of(firstPart));
            var callerFuture = first.toCompletableFuture();
            var failure = new IllegalStateException("caller changed its future");
            try {
                assertFalse(callerFuture.complete(null));
                assertFalse(callerFuture.completeExceptionally(failure));
                callerFuture.obtrudeException(failure);
                assertSame(
                        failure,
                        assertThrows(CompletionException.class, callerFuture::join).getCause());

                var nextSocket =
                        binding.send(Adapter.SOCKET, List.of(nextSocketPart)).toCompletableFuture();
                var nextService =
                        binding.send(Adapter.SERVICE, List.of(nextServicePart))
                                .toCompletableFuture();
                assertAll(
                        () -> assertTrue(nextSocket.isDone()),
                        () -> assertTrue(nextService.isDone()),
                        () -> assertNull(nextSocket.join()),
                        () -> assertNull(nextService.join()));

                callerFuture.obtrudeValue(null);
                assertNull(callerFuture.join());
                assertNull(nextSocket.join());
                assertNull(nextService.join());
                assertEquals(3, binding.submissions);
                assertEquals(0, binding.admissionReads);
            } finally {
                // Keep the shared-future regression from contaminating other tests.
                callerFuture.obtrudeValue(null);
            }
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void backpressuredSendWaitsForTheBindingWithoutResubmitting(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("send")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var completed = binding.send(adapter, List.of(part)).toCompletableFuture();

            assertFalse(completed.isDone());
            assertEquals(1, binding.resultReads);
            assertEquals(1, binding.admissionReads);
            binding.admission.complete(null);

            assertNull(completed.join());
            assertEquals(1, binding.submissions, "only the binding may retry its packet");
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void backpressuredSendPropagatesAdmissionFailureWithoutRetry(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("send")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var completed = binding.send(adapter, List.of(part)).toCompletableFuture();
            var failure = new IllegalStateException("binding admission failed");
            binding.admission.completeExceptionally(failure);

            assertSame(
                    failure, assertThrows(CompletionException.class, completed::join).getCause());
            assertEquals(1, binding.admissionReads);
            assertEquals(1, binding.submissions);
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void pendingReplyDoesNotBlockTheNextRequestOrReorderItsReply(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message firstPart = Message.from("first");
                Message nextPart = Message.from("next")) {
            var firstReply = binding.request(adapter, List.of(firstPart)).toCompletableFuture();
            var nextReply = binding.request(adapter, List.of(nextPart)).toCompletableFuture();

            assertEquals(2, binding.submissions);
            assertEquals(0, binding.admissionReads, "OK must not inspect admission");
            assertFalse(firstReply.isDone());
            assertFalse(nextReply.isDone());
            binding.replies.get(1).complete(List.of(Message.from("next-reply")));
            assertArrayEquals(
                    "next-reply".getBytes(java.nio.charset.StandardCharsets.UTF_8),
                    nextReply.join());
            assertFalse(firstReply.isDone());
            binding.replies.get(0).complete(List.of(Message.from("first-reply")));
            assertArrayEquals(
                    "first-reply".getBytes(java.nio.charset.StandardCharsets.UTF_8),
                    firstReply.join());
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void requestAdmissionDoesNotCompleteItsReply(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("request")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var reply = binding.request(adapter, List.of(part)).toCompletableFuture();
            assertFalse(reply.isDone());
            assertEquals(1, binding.resultReads);
            assertEquals(1, binding.admissionReads);
            assertEquals(1, binding.replyReads);
            assertFalse(binding.replies.getFirst().isDone());
            binding.admission.complete(null);
            assertFalse(reply.isDone(), "admission is not a request terminal");
            assertEquals(1, binding.replyReads);
            var failure = new IllegalStateException("binding reply failed");
            binding.replies.getFirst().completeExceptionally(failure);
            assertSame(failure, assertThrows(CompletionException.class, reply::join).getCause());
            assertEquals(1, binding.submissions);
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void cancellingPendingRequestCancelsBindingWaiterWithoutResubmission(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("request")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var completion = requestForCancellation(binding, adapter, List.of(part));
            assertTrue(completion.cancel(false));
            assertTrue(binding.replies.getFirst().isCancelled());
            assertEquals(1, binding.submissions);
            binding.admission.complete(null);
            assertTrue(completion.isCancelled());
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void bindingReplyWinningBeforeCancellationKeepsItsResult(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("request")) {
            var completion = binding.request(adapter, List.of(part)).toCompletableFuture();
            binding.replies.getFirst().complete(List.of(Message.from("reply")));
            assertArrayEquals(
                    "reply".getBytes(java.nio.charset.StandardCharsets.UTF_8), completion.join());
            assertFalse(completion.cancel(false));
            assertEquals(1, binding.submissions);
        }
    }

    private static CompletableFuture<?> requestForCancellation(
            BindingProbe binding, Adapter adapter, List<Message> parts) {
        return adapter == Adapter.SERVICE
                ? binding.request(adapter, parts).toCompletableFuture()
                : ZLinkJavaSocketSupport.submitRequest(
                                binding.requestOperation(), parts, TIMEOUT, null, null)
                        .toCompletableFuture();
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void lateBindingReplyAfterCallerCancellationClosesItsPayload(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("request")) {
            binding.result = SubmitResult.BACKPRESSURED;
            binding.ignoreReplyCancellation = true;
            var completion = requestForCancellation(binding, adapter, List.of(part));
            assertTrue(completion.cancel(false));
            binding.admission.complete(null);
            try (Message late = Message.from("late")) {
                assertTrue(binding.replies.getFirst().complete(List.of(late)));
                assertTrue(late.empty());
            }
            assertTrue(completion.isCancelled());
        }
    }

    @Test
    void cancelledServiceRequestDiscardsALateDecodedReply() {
        try (var binding = new BindingProbe();
                Message part = Message.from("request");
                Message decoded = Message.from("decoded")) {
            CountDownLatch decoderEntered = new CountDownLatch(1);
            CountDownLatch releaseDecoder = new CountDownLatch(1);
            var completion =
                    binding.port
                            .requestMessages(
                                    binding.router,
                                    TARGET,
                                    List.of(part),
                                    TIMEOUT,
                                    ignored -> {
                                        decoderEntered.countDown();
                                        try {
                                            assertTrue(releaseDecoder.await(5, TimeUnit.SECONDS));
                                        } catch (InterruptedException failure) {
                                            Thread.currentThread().interrupt();
                                            throw new AssertionError(failure);
                                        }
                                        return new ZLinkBackendReceived(
                                                ZLinkBackendRequestResult.OK,
                                                Optional.empty(),
                                                Optional.empty(),
                                                Optional.empty(),
                                                List.of(decoded));
                                    },
                                    ZLinkBackendReceived::close,
                                    null,
                                    null)
                            .toCompletableFuture();
            var delivery =
                    CompletableFuture.runAsync(
                            () ->
                                    assertTrue(
                                            binding.replies
                                                    .getFirst()
                                                    .complete(List.of(Message.from("wire")))));
            try {
                assertTrue(decoderEntered.await(5, TimeUnit.SECONDS));
                assertTrue(completion.cancel(false));
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                throw new AssertionError(failure);
            } finally {
                releaseDecoder.countDown();
            }
            delivery.join();
            assertTrue(decoded.empty());
            assertTrue(completion.isCancelled());
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void requestWithoutCapacityWaitTokenFailsUnavailableWithoutQueuing(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("request")) {
            binding.requestFailure = new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
            var reply = binding.request(adapter, List.of(part)).toCompletableFuture();
            var failure =
                    assertInstanceOf(
                            ZLinkFrameworkException.class,
                            assertThrows(CompletionException.class, reply::join).getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, failure.kind());
            assertSame(binding.requestFailure, failure.getCause());
            assertEquals(1, binding.submissions);
            assertEquals(0, binding.resultReads);
            assertEquals(0, binding.admissionReads);
            assertEquals(0, binding.replyReads);
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void typedInitialRequestFailuresAreProjectedWithTheirOriginalCause(Adapter adapter) {
        for (var result : List.of(SubmitResult.INTERNAL_ERROR, SubmitResult.TERMINATED)) {
            try (var binding = new BindingProbe();
                    Message part = Message.from("request")) {
                var cause = new ZlinkSubmitException(result);
                binding.requestFailure = cause;
                var reply = binding.request(adapter, List.of(part)).toCompletableFuture();
                var failure = assertThrows(CompletionException.class, reply::join).getCause();
                var framework = assertInstanceOf(ZLinkFrameworkException.class, failure);
                assertEquals(
                        result == SubmitResult.TERMINATED
                                ? ZLinkFrameworkErrorKind.SHUTTING_DOWN
                                : ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                        framework.kind());
                assertSame(cause, framework.getCause());
            }
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void unknownInitialRequestFailuresKeepTheirOriginalCause(Adapter adapter) {
        try (var binding = new BindingProbe();
                Message part = Message.from("request")) {
            var cause = new IllegalStateException("unknown initial request failure");
            binding.requestFailure = cause;
            var reply = binding.request(adapter, List.of(part)).toCompletableFuture();
            assertSame(cause, assertThrows(CompletionException.class, reply::join).getCause());
        }
    }

    @Test
    void closingServicePortEndsBackpressuredSendBeforeLateBindingTerminal() throws Exception {
        try (var binding = new BindingProbe();
                Message part = Message.from("pending")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var caller = binding.send(Adapter.SERVICE, List.of(part)).toCompletableFuture();
            var terminal = new CompletableFuture<Throwable>();
            ZLinkOneWayCalls.adaptOneWay(caller)
                    .whenComplete((ignored, failure) -> terminal.complete(failure));
            binding.port.close();
            var failure = terminal.get(5, TimeUnit.SECONDS);
            var framework = assertInstanceOf(ZLinkFrameworkException.class, failure);
            assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, framework.kind());
            assertTrue(binding.admission.complete(null));
            assertSame(failure, assertThrows(CompletionException.class, caller::join).getCause());
            assertEquals(1, binding.submissions);
        }
    }

    @Test
    void cancellingSendWaitDoesNotCancelBindingAdmission() {
        try (var binding = new BindingProbe();
                Message part = Message.from("pending")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var stage = binding.send(Adapter.SOCKET, List.of(part));
            assertTrue(stage.toCompletableFuture().cancel(false));
            assertFalse(binding.admission.isDone());
            binding.admission.complete(null);
            assertNull(stage.toCompletableFuture().join());
        }
    }

    @Test
    void pendingSendCallbackLeavesSourceLaneAndCanReenterRuntime() throws Exception {
        try (var binding = new BindingProbe();
                Message part = Message.from("pending")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var stage = binding.send(Adapter.SERVICE, List.of(part));
            var callback = new CompletableFuture<Boolean>();
            stage.whenComplete(
                    (ignored, failure) -> {
                        try {
                            assertFalse(binding.lane.isOnLane());
                            binding.result = SubmitResult.OK;
                            assertNull(
                                    binding.send(Adapter.SERVICE, List.of(Message.from("again")))
                                            .toCompletableFuture()
                                            .join());
                            callback.complete(true);
                        } catch (Throwable error) {
                            callback.completeExceptionally(error);
                        }
                    });
            binding.port.inStateLane(() -> binding.admission.complete(null));
            assertTrue(callback.get(5, TimeUnit.SECONDS));
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void onlyBackpressuredSendsRegisterAndRegisterBeforeObservingAdmission(Adapter adapter)
            throws Exception {
        try (var binding = new BindingProbe()) {
            long before = registrations(binding.operations);
            binding.send(adapter, List.of(Message.from("ok"))).toCompletableFuture().join();
            assertEquals(before, registrations(binding.operations));
            binding.sendFailure = new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
            try {
                var rejected = binding.send(adapter, List.of(Message.from("rejected")));
                assertThrows(
                        CompletionException.class, () -> rejected.toCompletableFuture().join());
            } catch (ZlinkSubmitException expected) {
                assertSame(binding.sendFailure, expected);
            }
            assertEquals(before, registrations(binding.operations));
            assertEquals(0, binding.operations.pendingCount());
            binding.sendFailure = null;
            binding.result = SubmitResult.BACKPRESSURED;
            binding.beforeAdmissionRead = () -> assertEquals(1, binding.operations.pendingCount());
            binding.admission.complete(null);
            binding.send(adapter, List.of(Message.from("already admitted")))
                    .toCompletableFuture()
                    .join();
            assertEquals(before + 1, registrations(binding.operations));
            assertEquals(0, binding.operations.pendingCount());
        }
    }

    @Test
    void closeWaitsForSubmitAndRegistrationInTheSocketTurn() throws Exception {
        try (var binding = new BindingProbe();
                var workers = java.util.concurrent.Executors.newFixedThreadPool(2)) {
            binding.result = SubmitResult.BACKPRESSURED;
            var submitted = new CountDownLatch(1);
            var releaseSubmit = new CountDownLatch(1);
            var closeQueued = new CountDownLatch(1);
            binding.afterSend =
                    () -> {
                        submitted.countDown();
                        await(releaseSubmit);
                    };
            var caller =
                    workers.submit(
                            () -> binding.send(Adapter.SERVICE, List.of(Message.from("pending"))));
            assertTrue(submitted.await(5, TimeUnit.SECONDS));
            var closed =
                    workers.submit(
                            () -> {
                                closeQueued.countDown();
                                binding.port.close();
                            });
            try {
                assertTrue(closeQueued.await(5, TimeUnit.SECONDS));
                assertFalse(closed.isDone());
            } finally {
                releaseSubmit.countDown();
            }
            var result = caller.get(5, TimeUnit.SECONDS).toCompletableFuture();
            closed.get(5, TimeUnit.SECONDS);
            assertEquals(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(
                                    ZLinkFrameworkException.class,
                                    assertThrows(CompletionException.class, result::join)
                                            .getCause())
                            .kind());
            assertEquals(1, registrations(binding.operations));
            assertEquals(0, binding.operations.pendingCount());
            binding.admission.completeExceptionally(new IllegalStateException("late failure"));
            assertEquals(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(
                                    ZLinkFrameworkException.class,
                                    assertThrows(CompletionException.class, result::join)
                                            .getCause())
                            .kind());
        }
    }

    enum SocketLifetime {
        DEALER,
        ROUTER
    }

    @ParameterizedTest
    @EnumSource(SocketLifetime.class)
    void standaloneSocketCloseOwnsPendingSendAndDoesNotWaitForCallback(SocketLifetime lifetime)
            throws Exception {
        try (var binding = new BindingProbe()) {
            binding.result = SubmitResult.BACKPRESSURED;
            var dealerNative =
                    (systems.zlink.contracts.sockets.DealerSocket)
                            Proxy.newProxyInstance(
                                    systems.zlink.contracts.sockets.DealerSocket.class
                                            .getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.contracts.sockets.DealerSocket.class
                                    },
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "send" -> binding.sendOperation();
                                                case "close" -> null;
                                                default -> throw new AssertionError(method);
                                            });
            var dealer =
                    lifetime == SocketLifetime.DEALER
                            ? new ZLinkJavaDealerSocket(dealerNative)
                            : null;
            var router =
                    lifetime == SocketLifetime.ROUTER
                            ? new ZLinkJavaRouterSocket(binding.router)
                            : null;
            Object owner = dealer != null ? dealer : router;
            var registryField = owner.getClass().getDeclaredField("operations");
            registryField.setAccessible(true);
            var registry = (ZLinkServiceOperationRegistry) registryField.get(owner);
            var stage =
                    dealer != null
                            ? dealer.send(List.of(Message.from("pending")))
                            : router.send(TARGET, List.of(Message.from("pending")));
            assertEquals(1, registry.pendingCount());
            var callbackEntered = new CountDownLatch(1);
            var releaseCallback = new CountDownLatch(1);
            var observed =
                    ZLinkOneWayCalls.adaptOneWay(stage)
                            .handle(
                                    (ignored, failure) -> {
                                        assertFalse(Thread.holdsLock(owner));
                                        callbackEntered.countDown();
                                        await(releaseCallback);
                                        return failure;
                                    });
            try {
                if (dealer != null) dealer.close();
                else router.close();
                assertTrue(callbackEntered.await(5, TimeUnit.SECONDS));
                assertEquals(0, registry.pendingCount());
                assertTrue(binding.admission.complete(null));
            } finally {
                releaseCallback.countDown();
                if (dealer != null) dealer.close();
                else router.close();
            }
            assertEquals(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(
                                    ZLinkFrameworkException.class,
                                    observed.toCompletableFuture().get(5, TimeUnit.SECONDS))
                            .kind());
        }
    }

    @Test
    void meshAndPortShareExactlyOnePendingSendRegistry() throws Exception {
        try (var binding = new BindingProbe();
                var mesh = new ZLinkJavaRawMeshNode(binding.context, "pending-send-mesh")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var portField = ZLinkJavaRawMeshNode.class.getDeclaredField("port");
            portField.setAccessible(true);
            var meshPort = (ZLinkJavaRawServicePort) portField.get(mesh);
            var operationsField = ZLinkJavaRawMeshNode.class.getDeclaredField("operations");
            operationsField.setAccessible(true);
            var registry = (ZLinkServiceOperationRegistry) operationsField.get(mesh);
            var portOperations = ZLinkJavaRawServicePort.class.getDeclaredField("operations");
            portOperations.setAccessible(true);
            assertSame(registry, portOperations.get(meshPort));
            var socket = meshPort.openRouter(RoutingId.from("mesh-source"));
            var send = meshPort.sendMessages(socket, TARGET, List.of(Message.from("pending")));
            assertEquals(1, registry.pendingCount());
            assertEquals(1, registrations(registry));
            mesh.close();
            var failure =
                    assertThrows(CompletionException.class, () -> send.toCompletableFuture().join())
                            .getCause();
            assertEquals(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(ZLinkFrameworkException.class, failure).kind());
            binding.admission.completeExceptionally(new IllegalStateException("late route loss"));
            assertSame(
                    failure,
                    assertThrows(CompletionException.class, () -> send.toCompletableFuture().join())
                            .getCause());
            assertEquals(0, registry.pendingCount());
        }
    }

    private static long registrations(ZLinkServiceOperationRegistry registry) throws Exception {
        var slotsField = ZLinkServiceOperationRegistry.class.getDeclaredField("slots");
        slotsField.setAccessible(true);
        long count = 0;
        for (var entry : (Object[]) slotsField.get(registry)) {
            var generation = entry.getClass().getDeclaredField("generation");
            generation.setAccessible(true);
            count += generation.getLong(entry);
        }
        return count;
    }

    private static void await(CountDownLatch latch) {
        try {
            assertTrue(latch.await(5, TimeUnit.SECONDS));
        } catch (InterruptedException failure) {
            Thread.currentThread().interrupt();
            throw new AssertionError(failure);
        }
    }

    /** Controls completion delivery at the public binding boundary, without timers. */
    private static final class BindingProbe implements AutoCloseable {
        private final CompletableFuture<Void> admission = new CompletableFuture<>();
        private final List<CompletableFuture<List<Message>>> replies = new ArrayList<>();
        private final List<Message> parts = new ArrayList<>();
        private List<byte[]> frames;
        private SubmitResult result = SubmitResult.OK;
        private RuntimeException requestFailure;
        private RuntimeException sendFailure;
        private Runnable afterSend = () -> {};
        private Runnable beforeAdmissionRead = () -> {};
        private boolean ignoreReplyCancellation;
        private int resultReads;
        private int admissionReads;
        private int replyReads;
        private int submissions;
        private final systems.zlink.framework.runtime.internal.execution.ZLinkStateLane lane =
                new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane();
        private final ZLinkServiceOperationRegistry operations =
                new ZLinkServiceOperationRegistry(
                        ZLinkProcessExecutionLanes.deadlines(),
                        ZLinkOneWayCalls.failureForStatus(ZLinkOneWayCalls.SHUTDOWN));
        private final ZLinkJavaRawServicePort port;
        private final RouterSocket router;
        private final Context context;

        BindingProbe() {
            router =
                    (RouterSocket)
                            Proxy.newProxyInstance(
                                    RouterSocket.class.getClassLoader(),
                                    new Class<?>[] {RouterSocket.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "hashCode" -> System.identityHashCode(proxy);
                                                case "equals" -> proxy == args[0];
                                                case "setRoutingId", "close" -> null;
                                                case "send" -> sendOperation();
                                                case "request" -> requestOperation();
                                                default -> throw new AssertionError(method);
                                            });
            context =
                    (Context)
                            Proxy.newProxyInstance(
                                    Context.class.getClassLoader(),
                                    new Class<?>[] {Context.class},
                                    (proxy, method, args) -> {
                                        assertEquals("createRouterSocket", method.getName());
                                        return router;
                                    });
            port = new ZLinkJavaRawServicePort(context, lane, operations);
            assertSame(router, port.openRouter(RoutingId.from("submission-source")));
        }

        CompletionStage<Void> send(Adapter adapter, List<Message> messages) {
            return adapter == Adapter.SERVICE
                    ? port.sendMessages(router, TARGET, messages)
                    : ZLinkJavaSocketSupport.submit(sendOperation(), messages, operations);
        }

        CompletionStage<byte[]> request(Adapter adapter, List<Message> messages) {
            if (adapter == Adapter.SERVICE) {
                return port.requestMessages(
                        router,
                        TARGET,
                        messages,
                        TIMEOUT,
                        reply -> reply.getFirst().toByteArray(),
                        ignored -> {},
                        null,
                        null);
            }
            return ZLinkJavaSocketSupport.submitRequest(
                            requestOperation(), messages, TIMEOUT, null, null)
                    .thenApply(
                            reply -> {
                                try (reply) {
                                    return reply.parts().getFirst().toByteArray();
                                }
                            });
        }

        private SendOperation sendOperation() {
            var operation =
                    new SendSubmitOperation() {
                        @Override
                        public SendSubmitOperation message(Message part) {
                            parts.add(part);
                            return this;
                        }

                        @Override
                        public SendSubmission submit() {
                            consume();
                            if (sendFailure != null) throw sendFailure;
                            afterSend.run();
                            return new SendSubmission() {
                                @Override
                                public SubmitResult result() {
                                    resultReads++;
                                    return result;
                                }

                                @Override
                                public CompletionStage<Void> admitted() {
                                    return readAdmission();
                                }
                            };
                        }

                        @Override
                        public void submit_sync() {
                            fail("framework must use the asynchronous terminal");
                        }
                    };
            return operation::message;
        }

        private RequestOperation requestOperation() {
            var operation =
                    new RequestSubmitOperation() {
                        @Override
                        public RequestSubmitOperation message(Message part) {
                            parts.add(part);
                            return this;
                        }

                        @Override
                        public RequestSubmitOperation timeout(Duration timeout) {
                            assertEquals(TIMEOUT, timeout);
                            return this;
                        }

                        @Override
                        public RequestSubmission submit() {
                            consume();
                            if (requestFailure != null) {
                                throw requestFailure;
                            }
                            var reply =
                                    new CompletableFuture<List<Message>>() {
                                        @Override
                                        public boolean cancel(boolean mayInterruptIfRunning) {
                                            return !ignoreReplyCancellation
                                                    && super.cancel(mayInterruptIfRunning);
                                        }
                                    };
                            replies.add(reply);
                            return new RequestSubmission() {
                                @Override
                                public SubmitResult result() {
                                    resultReads++;
                                    return result;
                                }

                                @Override
                                public CompletionStage<Void> admitted() {
                                    return readAdmission();
                                }

                                @Override
                                public CompletionStage<List<Message>> reply() {
                                    replyReads++;
                                    return reply;
                                }
                            };
                        }

                        @Override
                        public List<Message> submit_sync() {
                            throw new AssertionError(
                                    "framework must use the asynchronous terminal");
                        }
                    };
            return operation::message;
        }

        private CompletionStage<Void> readAdmission() {
            beforeAdmissionRead.run();
            admissionReads++;
            assertEquals(
                    SubmitResult.BACKPRESSURED,
                    result,
                    "OK must use its snapshot without reading admitted()");
            return admission;
        }

        private void consume() {
            submissions++;
            frames = parts.stream().map(Message::toByteArray).toList();
            Message.closeAll(parts);
            parts.clear();
        }

        @Override
        public void close() {
            port.close();
        }
    }
}
