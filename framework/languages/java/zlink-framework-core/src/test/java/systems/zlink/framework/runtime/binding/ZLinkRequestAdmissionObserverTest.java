package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.sockets.SubmitResult;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;

final class ZLinkRequestAdmissionObserverTest {
    private static final int REQUESTS = 1000;

    private static final class Observed<T> extends CompletableFuture<T> {
        int observers;

        @Override
        public CompletableFuture<T> whenComplete(BiConsumer<? super T, ? super Throwable> action) {
            observers++;
            return super.whenComplete(action);
        }
    }

    @Test
    void absentAdmissionConsumerNeedsOnlyTheReplyObserver() {
        verify(false);
    }

    @Test
    void presentAdmissionConsumerReceivesSuccessAndReplyRemainsTheOnlyTerminal() {
        verify(true);
    }

    private static void verify(boolean consumesAdmission) {
        for (int outcome = 0; outcome < 3; outcome++) {
            int admissionObservers = 0, replyObservers = 0;
            for (int index = 0; index < REQUESTS; index++) {
                var admitted = new Observed<Void>();
                var reply = new Observed<List<Message>>();
                var consumer = consumesAdmission ? new CompletableFuture<Void>() : null;
                var terminals = new AtomicInteger();
                RequestSubmission submission =
                        new RequestSubmission() {
                            public SubmitResult result() {
                                return SubmitResult.BACKPRESSURED;
                            }

                            public CompletableFuture<Void> admitted() {
                                return admitted;
                            }

                            public CompletableFuture<List<Message>> reply() {
                                return reply;
                            }
                        };
                assertSame(
                        reply,
                        ZLinkJavaSocketSupport.observeReply(
                                submission,
                                Long.MAX_VALUE,
                                null,
                                consumer,
                                (parts, failure) -> terminals.incrementAndGet()));
                if (outcome == 0) {
                    admitted.complete(null);
                    if (consumer != null) assertTrue(consumer.isDone());
                    reply.complete(List.of());
                } else if (outcome == 1) {
                    reply.cancel(false);
                } else {
                    var failure = new java.util.concurrent.TimeoutException("binding terminal");
                    admitted.completeExceptionally(failure);
                    reply.completeExceptionally(failure);
                }
                assertEquals(1, terminals.get());
                if (consumer != null && outcome != 0) assertFalse(consumer.isDone());
                admissionObservers += admitted.observers;
                replyObservers += reply.observers;
            }
            System.out.printf(
                    "requests=%d outcome=%d consumesAdmission=%s admissionObservers=%d"
                            + " replyObservers=%d%n",
                    REQUESTS, outcome, consumesAdmission, admissionObservers, replyObservers);
            assertEquals(consumesAdmission ? REQUESTS : 0, admissionObservers);
            assertEquals(REQUESTS, replyObservers);
        }
    }
}
