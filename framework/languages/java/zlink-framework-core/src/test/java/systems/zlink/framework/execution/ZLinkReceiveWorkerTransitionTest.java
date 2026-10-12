package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkReceiveWorkerTransitionTest {
    @Test
    void asynchronousWorkerIsPublishedWithoutAnIntermediateExecutor() throws Exception {
        Thread ingress = Thread.currentThread();
        AtomicReference<Thread> publisher = new AtomicReference<>();
        CountDownLatch published = new CountDownLatch(1);
        Executor worker =
                command -> {
                    publisher.set(Thread.currentThread());
                    published.countDown();
                };
        var queue = new ZLinkSerialExecutionQueue(worker, ZLinkExecutionLanePolicy.generic());
        try {
            queue.enqueue(() -> CompletableFuture.completedFuture(null), null);
            if (!published.await(3, TimeUnit.SECONDS))
                throw new AssertionError("Worker was not published");
            assertSame(
                    ingress,
                    publisher.get(),
                    "claim-to-worker publication must not cross an intermediate executor");
        } finally {
            queue.close();
        }
    }
}
