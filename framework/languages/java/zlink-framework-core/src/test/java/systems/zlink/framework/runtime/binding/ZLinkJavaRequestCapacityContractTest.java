package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

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

final class ZLinkJavaRequestCapacityContractTest {
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
    void requestCapacityFailureUsesTheSubmissionPhase(boolean initialSubmission) {
        var failure = new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
        assertEquals(
                initialSubmission ? RequestResult.NOT_CONNECTED : RequestResult.TIMED_OUT,
                ZLinkJavaRawMeshNode.requestResult(failure, initialSubmission));
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
            assertEquals(
                    initialSubmission
                            ? ZLinkFrameworkErrorKind.UNAVAILABLE
                            : ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    terminal.kind());
            assertSame(failure, terminal.getCause());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void wrappedFailuresUseTheTypedRequestProjection(boolean initialSubmission) {
        assertNull(
                ZLinkJavaRawMeshNode.requestResult(
                        new CompletionException(new IllegalStateException("unknown")),
                        initialSubmission));
        assertEquals(
                RequestResult.INTERNAL_ERROR,
                ZLinkJavaRawMeshNode.requestResult(
                        new CompletionException(
                                new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR)),
                        initialSubmission));
        assertEquals(
                RequestResult.INTERNAL_ERROR,
                ZLinkJavaRawMeshNode.requestResult(
                        new CompletionException(
                                new ZlinkRequestException(RequestResult.INTERNAL_ERROR)),
                        initialSubmission));
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
