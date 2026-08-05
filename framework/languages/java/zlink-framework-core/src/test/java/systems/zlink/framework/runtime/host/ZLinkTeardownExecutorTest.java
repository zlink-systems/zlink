package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

final class ZLinkTeardownExecutorTest {
    @Test
    void providerCompletionThreadDoesNotRunTeardownCleanup() throws Exception {
        String callbackThread = Thread.currentThread().getName();
        AtomicReference<String> cleanupThread = new AtomicReference<>();
        CountDownLatch cleanupCompleted = new CountDownLatch(1);

        ZLinkTeardownExecutor.execute(() -> {
            cleanupThread.set(Thread.currentThread().getName());
            cleanupCompleted.countDown();
        });

        assertTrue(cleanupCompleted.await(1, TimeUnit.SECONDS));
        assertNotEquals(callbackThread, cleanupThread.get());
        assertTrue(cleanupThread.get().startsWith("zlink-framework-teardown"));
    }
}
