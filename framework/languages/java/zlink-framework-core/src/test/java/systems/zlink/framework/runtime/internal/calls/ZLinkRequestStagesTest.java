package systems.zlink.framework.runtime.internal.calls;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;

class ZLinkRequestStagesTest {
    @Test
    void successfulReplyCompletesMissingAdmissionObservation() {
        var reply = new CompletableFuture<String>();
        var admission = new CompletableFuture<Void>();
        var stage = ZLinkRequestStages.observe(reply, admission);
        assertFalse(stage.admitted().toCompletableFuture().isDone());
        reply.complete("reply");
        assertTrue(stage.admitted().toCompletableFuture().isDone());
        assertNull(stage.admitted().toCompletableFuture().join());
        assertEquals("reply", stage.toCompletableFuture().join());
    }

    @Test
    void successfulReplyDoesNotReplaceEarlierAdmissionFailure() {
        var reply = new CompletableFuture<String>();
        var admission = new CompletableFuture<Void>();
        var stage = ZLinkRequestStages.observe(reply, admission);
        var failure = new IllegalStateException("admission failed");
        admission.completeExceptionally(failure);
        reply.complete("reply");
        var observed =
                assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () -> stage.admitted().toCompletableFuture().join());
        assertSame(failure, observed.getCause());
    }
}
