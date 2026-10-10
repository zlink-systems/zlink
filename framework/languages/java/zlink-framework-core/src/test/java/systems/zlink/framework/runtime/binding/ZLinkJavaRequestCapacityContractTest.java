package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class ZLinkJavaRequestCapacityContractTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void rawMeshServiceRequestUsesTheSameAdmissionDeadline(boolean admitBeforeDeadline)
            throws Exception {
        var request = new PendingRequest(SubmitResult.BACKPRESSURED);
        var router =
                (systems.zlink.contracts.sockets.RouterSocket)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.sockets.RouterSocket.class.getClassLoader(),
                                new Class<?>[] {systems.zlink.contracts.sockets.RouterSocket.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "hashCode" -> System.identityHashCode(proxy);
                                            case "equals" -> proxy == args[0];
                                            case "setRoutingId", "close" -> null;
                                            case "request" -> request;
                                            default -> throw new AssertionError(method.getName());
                                        });
        var context =
                (systems.zlink.contracts.core.Context)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.core.Context.class.getClassLoader(),
                                new Class<?>[] {systems.zlink.contracts.core.Context.class},
                                (proxy, method, args) -> {
                                    assertEquals("createRouterSocket", method.getName());
                                    return router;
                                });
        try (var port = new ZLinkJavaRawServicePort(context)) {
            var caller = port.openRouter(systems.zlink.contracts.core.RoutingId.from("caller"));
            var completion =
                    port.request(
                                    caller,
                                    systems.zlink.contracts.core.RoutingId.from("target"),
                                    List.of(new byte[] {1}),
                                    Duration.ofMillis(100))
                            .toCompletableFuture();
            if (admitBeforeDeadline) request.admitted.complete(null);
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> completion.get(1, TimeUnit.SECONDS));
            assertEquals(
                    RequestResult.TIMED_OUT,
                    assertInstanceOf(ZlinkRequestException.class, failure.getCause()).getResult());
            assertFalse(request.reply.isDone(), "binding completion must remain active");
            request.admitted.complete(null);
            var lateReply = Message.from(new byte[] {2});
            request.completeReply(List.of(lateReply));
            assertEquals(0, lateReply.size());
        }
    }

    @Test
    void backpressuredRequestExpiresBeforeAdmissionAndBindingDiscardsLateReply() throws Exception {
        assertBackpressuredDeadline(false);
    }

    @Test
    void backpressuredRequestKeepsOriginalDeadlineAfterAdmission() throws Exception {
        assertBackpressuredDeadline(true);
    }

    private void assertBackpressuredDeadline(boolean admitBeforeDeadline) throws Exception {
        var request = new PendingRequest(SubmitResult.BACKPRESSURED);
        try (var message = Message.from(new byte[] {1})) {
            var completion =
                    ZLinkJavaSocketSupport.submitRequest(
                                    request, List.of(message), Duration.ofMillis(100))
                            .toCompletableFuture();
            if (admitBeforeDeadline) request.admitted.complete(null);
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> completion.get(1, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertFalse(request.reply.isDone(), "binding completion must remain active");
            if (!admitBeforeDeadline) {
                assertFalse(request.admitted.isDone(), "caller deadline must not stop admission");
                request.admitted.complete(null);
            }
            var lateReply = Message.from(new byte[] {2});
            request.completeReply(List.of(lateReply));
            assertEquals(0, lateReply.size(), "binding must close an unaccepted late reply");
        }
    }

    @Test
    void immediatelyAdmittedRequestHasNoFrameworkDeadline() throws Exception {
        var request = new PendingRequest(SubmitResult.OK);
        try (var message = Message.from(new byte[] {1})) {
            var completion =
                    ZLinkJavaSocketSupport.submitRequest(
                                    request, List.of(message), Duration.ofMillis(30))
                            .toCompletableFuture();
            assertThrows(TimeoutException.class, () -> completion.get(150, TimeUnit.MILLISECONDS));
            assertFalse(request.reply.isCancelled());
            request.completeReply(List.of(Message.from(new byte[] {2})));
            try (var received = completion.get(1, TimeUnit.SECONDS)) {
                assertEquals(1, received.parts().size());
            }
        }
    }

    private static final class PendingRequest implements RequestOperation, RequestSubmitOperation {
        private final SubmitResult result;
        private final CompletableFuture<Void> admitted = new CompletableFuture<>();
        private final CompletableFuture<List<Message>> reply = new CompletableFuture<>();

        private PendingRequest(SubmitResult result) {
            this.result = result;
        }

        void completeReply(List<Message> parts) {
            assertTrue(reply.complete(parts), "binding must still deliver its completion");
        }

        @Override
        public RequestSubmitOperation message(Message part) {
            return this;
        }

        @Override
        public RequestSubmitOperation timeout(Duration timeout) {
            return this;
        }

        @Override
        public RequestSubmission submit() {
            return new RequestSubmission() {
                @Override
                public SubmitResult result() {
                    return result;
                }

                @Override
                public CompletionStage<Void> admitted() {
                    return admitted;
                }

                @Override
                public CompletionStage<List<Message>> reply() {
                    return reply;
                }
            };
        }

        @Override
        public List<Message> submit_sync() {
            throw new UnsupportedOperationException();
        }
    }

    @ParameterizedTest
    @CsvSource({
        "NOT_ADMITTED, REJECTED",
        "NOT_CONNECTED, UNAVAILABLE",
        "NOT_FOUND, NOT_FOUND",
        "TERMINATED, SHUTTING_DOWN",
        "INVALID_ARGUMENT, INVALID_OPERATION",
        "INVALID_HANDLE, INVALID_OPERATION",
        "INVALID_STATE, INVALID_OPERATION",
        "THREAD_VIOLATION, INVALID_OPERATION",
        "NOT_SUPPORTED, INTERNAL_FAILURE",
        "OUT_OF_MEMORY, INTERNAL_FAILURE",
        "SEQ_EXHAUSTED, INTERNAL_FAILURE",
        "INTERNAL_ERROR, INTERNAL_FAILURE"
    })
    void submitFailuresPreserveTheirTypedMeaning(
            SubmitResult result, ZLinkFrameworkErrorKind expected) {
        for (boolean initial : new boolean[] {true, false}) {
            var failure = new ZlinkSubmitException(result);
            var reply = new CompletableFuture<List<Message>>();
            var request = new CapacityRejectedRequest(initial, failure, reply);
            try (var message = Message.from(new byte[] {1})) {
                var completion =
                        ZLinkJavaSocketSupport.submitRequest(
                                        request, List.of(message), Duration.ofSeconds(1))
                                .toCompletableFuture();
                if (!initial) reply.completeExceptionally(failure);
                var terminal =
                        assertInstanceOf(
                                ZLinkFrameworkException.class,
                                assertThrows(CompletionException.class, completion::join)
                                        .getCause());
                assertEquals(expected, terminal.kind());
                assertSame(failure, terminal.getCause());
            }
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void tokenlessRequestCapacityFailureIsUnavailableAtEitherBoundary(boolean initialSubmission) {
        var failure = new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
        assertEquals(RequestResult.BACKPRESSURED, ZLinkJavaRawMeshNode.requestResult(failure));
        var reply = new CompletableFuture<List<Message>>();
        var request = new CapacityRejectedRequest(initialSubmission, failure, reply);
        try (var message = Message.from(new byte[] {1})) {
            var completion =
                    ZLinkJavaSocketSupport.submitRequest(
                                    request, List.of(message), Duration.ofSeconds(1))
                            .toCompletableFuture();
            if (!initialSubmission) {
                assertFalse(completion.isDone());
                reply.completeExceptionally(failure);
            }
            assertTrue(completion.isDone());
            if (initialSubmission) {
                assertFalse(reply.isDone(), "a slot rejection must not wait for a reply");
            }
            var terminal =
                    assertInstanceOf(
                            ZLinkFrameworkException.class,
                            assertThrows(CompletionException.class, completion::join).getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, terminal.kind());
            assertTrue(terminal.getMessage().contains("submission capacity is unavailable"));
            assertSame(failure, terminal.getCause());
        }
        var oneWay =
                systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls.adaptOneWay(
                                CompletableFuture.failedFuture(failure), initialSubmission)
                        .toCompletableFuture();
        var oneWayFailure =
                assertInstanceOf(
                        ZLinkFrameworkException.class,
                        assertThrows(CompletionException.class, oneWay::join).getCause());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, oneWayFailure.kind());
        assertSame(failure, oneWayFailure.getCause());
    }

    @Test
    void wrappedFailuresUseTheTypedRequestProjection() {
        assertNull(
                ZLinkJavaRawMeshNode.requestResult(
                        new CompletionException(new IllegalStateException("unknown"))));
        assertEquals(
                RequestResult.INTERNAL_ERROR,
                ZLinkJavaRawMeshNode.requestResult(
                        new CompletionException(
                                new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR))));
        assertEquals(
                RequestResult.INTERNAL_ERROR,
                ZLinkJavaRawMeshNode.requestResult(
                        new CompletionException(
                                new ZlinkRequestException(RequestResult.INTERNAL_ERROR))));
    }

    @ParameterizedTest
    @CsvSource({"true, false", "true, true", "false, false", "false, true"})
    void unknownRequestFailuresKeepTheirOriginalCause(boolean initialSubmission, boolean wrapped) {
        var original = new IllegalStateException("request fixture failure");
        RuntimeException failure = wrapped ? new CompletionException(original) : original;
        var reply = new CompletableFuture<List<Message>>();
        var request = new CapacityRejectedRequest(initialSubmission, failure, reply);
        try (var message = Message.from(new byte[] {1})) {
            var completion =
                    ZLinkJavaSocketSupport.submitRequest(
                                    request, List.of(message), Duration.ofSeconds(1))
                            .toCompletableFuture();
            if (!initialSubmission) reply.completeExceptionally(failure);
            var terminal = assertThrows(CompletionException.class, completion::join);
            assertSame(failure, initialSubmission && wrapped ? terminal : terminal.getCause());
        }
    }

    private record CapacityRejectedRequest(
            boolean initialSubmission,
            RuntimeException failure,
            CompletableFuture<List<Message>> reply)
            implements RequestOperation, RequestSubmitOperation {
        @Override
        public RequestSubmitOperation message(Message part) {
            return this;
        }

        @Override
        public RequestSubmitOperation timeout(Duration timeout) {
            return this;
        }

        @Override
        public RequestSubmission submit() {
            if (initialSubmission) {
                throw failure;
            }
            return new RequestSubmission() {
                @Override
                public SubmitResult result() {
                    return SubmitResult.BACKPRESSURED;
                }

                @Override
                public CompletionStage<Void> admitted() {
                    return reply.thenApply(ignored -> null);
                }

                @Override
                public CompletionStage<List<Message>> reply() {
                    return reply;
                }
            };
        }

        @Override
        public List<Message> submit_sync() {
            throw new UnsupportedOperationException();
        }
    }
}
