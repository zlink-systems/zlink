package systems.zlink.runtime.nativeapi;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

public class RequestReplySupportTest {
    @Test
    public void blockedCompletionDoesNotDelayAnIndependentReply() throws Exception {
        CountDownLatch firstStarted = new CountDownLatch(1);
        CountDownLatch releaseFirst = new CountDownLatch(1);
        CompletableFuture<String> first = new CompletableFuture<>();
        CompletableFuture<String> second = new CompletableFuture<>();

        first.thenRun(() -> {
            firstStarted.countDown();
            await(releaseFirst);
        });

        RequestReplySupport.completeAsync(first, () -> "first");
        assertTrue(firstStarted.await(3, TimeUnit.SECONDS));

        RequestReplySupport.completeAsync(second, () -> "second");
        assertEquals("second", second.get(3, TimeUnit.SECONDS));
        assertFalse(releaseFirst.await(10, TimeUnit.MILLISECONDS));

        releaseFirst.countDown();
        assertEquals("first", first.get(3, TimeUnit.SECONDS));
    }

    private static void await(CountDownLatch latch) {
        try {
            if (!latch.await(3, TimeUnit.SECONDS)) {
                throw new AssertionError("timed out");
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError(interrupted);
        }
    }
}
