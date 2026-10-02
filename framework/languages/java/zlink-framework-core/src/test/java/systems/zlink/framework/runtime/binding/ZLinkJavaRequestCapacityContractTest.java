package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
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
    @ValueSource(booleans = {true, false})
    void requestCapacityFailureUsesTheSubmissionPhase(boolean initialSubmission) {
        var failure = new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
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

    private record CapacityRejectedRequest(
            boolean initialSubmission,
            ZlinkSubmitException failure,
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
