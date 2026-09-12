package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

final class ZLinkJavaSubmissionTest {
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    private static final RoutingId TARGET = RoutingId.from("submission-target");

    enum Adapter { SOCKET, SERVICE }

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
                assertSame(failure,
                    assertThrows(CompletionException.class, callerFuture::join).getCause());

                var nextSocket = binding.send(Adapter.SOCKET, List.of(nextSocketPart))
                    .toCompletableFuture();
                var nextService = binding.send(Adapter.SERVICE, List.of(nextServicePart))
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
        try (var binding = new BindingProbe(); Message part = Message.from("send")) {
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
        try (var binding = new BindingProbe(); Message part = Message.from("send")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var completed = binding.send(adapter, List.of(part)).toCompletableFuture();
            var failure = new IllegalStateException("binding admission failed");
            binding.admission.completeExceptionally(failure);

            assertSame(failure, assertThrows(CompletionException.class, completed::join).getCause());
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
            assertArrayEquals("next-reply".getBytes(java.nio.charset.StandardCharsets.UTF_8),
                nextReply.join());
            assertFalse(firstReply.isDone());
            binding.replies.get(0).complete(List.of(Message.from("first-reply")));
            assertArrayEquals("first-reply".getBytes(java.nio.charset.StandardCharsets.UTF_8),
                firstReply.join());
        }
    }

    @ParameterizedTest
    @EnumSource(Adapter.class)
    void requestAdmissionDoesNotCompleteItsReply(Adapter adapter) {
        try (var binding = new BindingProbe(); Message part = Message.from("request")) {
            binding.result = SubmitResult.BACKPRESSURED;
            var reply = binding.request(adapter, List.of(part)).toCompletableFuture();
            assertFalse(reply.isDone());
            binding.admission.complete(null);
            assertFalse(reply.isDone(), "admission is not a request terminal");
            var failure = new IllegalStateException("binding reply failed");
            binding.replies.getFirst().completeExceptionally(failure);
            assertSame(failure, assertThrows(CompletionException.class, reply::join).getCause());
            assertEquals(1, binding.submissions);
        }
    }

    /** Controls completion delivery at the public binding boundary, without timers. */
    private static final class BindingProbe implements AutoCloseable {
        private final CompletableFuture<Void> admission = new CompletableFuture<>();
        private final List<CompletableFuture<List<Message>>> replies = new ArrayList<>();
        private final List<Message> parts = new ArrayList<>();
        private List<byte[]> frames;
        private SubmitResult result = SubmitResult.OK;
        private int resultReads;
        private int admissionReads;
        private int submissions;
        private final ZLinkJavaRawServicePort port;
        private final RouterSocket router;

        BindingProbe() {
            router = (RouterSocket) Proxy.newProxyInstance(
                RouterSocket.class.getClassLoader(), new Class<?>[] {RouterSocket.class},
                (proxy, method, args) -> switch (method.getName()) {
                    case "hashCode" -> System.identityHashCode(proxy);
                    case "equals" -> proxy == args[0];
                    case "setRoutingId", "close" -> null;
                    case "send" -> sendOperation();
                    case "request" -> requestOperation();
                    default -> throw new AssertionError(method);
                });
            Context context = (Context) Proxy.newProxyInstance(
                Context.class.getClassLoader(), new Class<?>[] {Context.class},
                (proxy, method, args) -> {
                    assertEquals("createRouterSocket", method.getName());
                    return router;
                });
            port = new ZLinkJavaRawServicePort(context);
            assertSame(router, port.openRouter(RoutingId.from("submission-source")));
        }

        CompletionStage<Void> send(Adapter adapter, List<Message> messages) {
            return adapter == Adapter.SERVICE
                ? port.sendMessages(router, TARGET, messages)
                : ZLinkJavaSocketSupport.submit(sendOperation(), messages);
        }

        CompletionStage<byte[]> request(Adapter adapter, List<Message> messages) {
            if (adapter == Adapter.SERVICE) {
                return port.requestMessages(router, TARGET, messages, TIMEOUT,
                    reply -> reply.getFirst().toByteArray());
            }
            return ZLinkJavaSocketSupport.submitRequest(requestOperation(), messages, TIMEOUT)
                .thenApply(reply -> {
                    try (reply) {
                        return reply.parts().getFirst().toByteArray();
                    }
                });
        }

        private SendOperation sendOperation() {
            var operation = new SendSubmitOperation() {
                @Override public SendSubmitOperation message(Message part) {
                    parts.add(part);
                    return this;
                }
                @Override public SendSubmission submit() {
                    consume();
                    return new SendSubmission() {
                        @Override public SubmitResult result() {
                            resultReads++;
                            return result;
                        }
                        @Override public CompletionStage<Void> admitted() {
                            return readAdmission();
                        }
                    };
                }
                @Override public void submit_sync() {
                    fail("framework must use the asynchronous terminal");
                }
            };
            return operation::message;
        }

        private RequestOperation requestOperation() {
            var operation = new RequestSubmitOperation() {
                @Override public RequestSubmitOperation message(Message part) {
                    parts.add(part);
                    return this;
                }
                @Override public RequestSubmitOperation timeout(Duration timeout) {
                    assertEquals(TIMEOUT, timeout);
                    return this;
                }
                @Override public RequestSubmission submit() {
                    consume();
                    var reply = new CompletableFuture<List<Message>>();
                    replies.add(reply);
                    return new RequestSubmission() {
                        @Override public SubmitResult result() { return result; }
                        @Override public CompletionStage<Void> admitted() {
                            return readAdmission();
                        }
                        @Override public CompletionStage<List<Message>> reply() { return reply; }
                    };
                }
                @Override public List<Message> submit_sync() {
                    throw new AssertionError("framework must use the asynchronous terminal");
                }
            };
            return operation::message;
        }

        private CompletionStage<Void> readAdmission() {
            admissionReads++;
            assertEquals(SubmitResult.BACKPRESSURED, result,
                "OK must use its snapshot without reading admitted()");
            return admission;
        }

        private void consume() {
            submissions++;
            frames = parts.stream().map(Message::toByteArray).toList();
            Message.closeAll(parts);
            parts.clear();
        }

        @Override public void close() { port.close(); }
    }
}
