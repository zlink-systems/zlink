package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.channels.ZLinkRequestCallerAllocationTest;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;
import java.util.function.Supplier;

/** Admission forwarding and reply delivery need two closures, without a captured getter. */
public final class ZLinkRequestAdmissionAdapterAllocationTest {
    private static final int REQUESTS = 1000;
    private static volatile Object observed;

    private static final class Submission implements RequestSubmission {
        final CompletableFuture<Void> admitted = new CompletableFuture<>();
        final CompletableFuture<List<Message>> reply = new CompletableFuture<>();
        int admissionReads;

        public SubmitResult result() {
            return SubmitResult.BACKPRESSURED;
        }

        public CompletionStage<Void> admitted() {
            admissionReads++;
            return admitted;
        }

        public CompletionStage<List<Message>> reply() {
            return reply;
        }
    }

    @Test
    void admissionGetterDoesNotNeedAnAdapterAllocation() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    private static Supplier<CompletionStage<Void>> capturedAccessor(Submission submission) {
        return submission::admitted;
    }

    private static void submit(
            Submission submission, BiConsumer<List<Message>, Throwable> terminal) {
        var consumer = new CompletableFuture<Void>();
        observed =
                ZLinkJavaSocketSupport.observeReply(
                        submission, Long.MAX_VALUE, null, consumer, terminal);
        assertEquals(1, submission.admissionReads);
        submission.admitted.complete(null);
        assertTrue(consumer.isDone());
        submission.reply.complete(List.of());
    }

    public static void main(String[] args) throws Exception {
        var controlSubmission = new Submission();
        for (int index = 0; index < 200; index++) {
            observed = capturedAccessor(controlSubmission);
            submit(new Submission(), (parts, error) -> {});
        }
        var control =
                ZLinkRequestCallerAllocationTest.allocations(
                        ZLinkRequestAdmissionAdapterAllocationTest.class,
                        List.of("capturedAccessor"),
                        () -> {
                            for (int i = 0; i < REQUESTS; i++)
                                observed = capturedAccessor(controlSubmission);
                        },
                        type -> type.contains("$$Lambda"));
        var terminals = new AtomicInteger();
        BiConsumer<List<Message>, Throwable> terminal =
                (parts, error) -> terminals.incrementAndGet();
        var actual =
                ZLinkRequestCallerAllocationTest.allocations(
                        ZLinkJavaSocketSupport.class,
                        List.of("observeReply"),
                        () -> {
                            for (int i = 0; i < REQUESTS; i++) submit(new Submission(), terminal);
                        },
                        type -> type.contains("$$Lambda"));
        System.out.printf(
                "requests=%d getterControl=%s frameworkClosures=%s terminals=%d%n",
                REQUESTS, control, actual, terminals.get());
        assertEquals(REQUESTS, control.objects());
        assertEquals(REQUESTS, terminals.get());
        assertEquals(
                2L * REQUESTS,
                actual.objects(),
                "one admission forwarder and one reply observer, without a getter adapter");
    }
}
