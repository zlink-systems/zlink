package systems.zlink.framework.runtime.internal.dispatch;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;

import java.util.ArrayList;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkLocalJobBacklogTest {
    private static final String IDENTIFIER = "zlink.runtime.host.local_job_backlog_exceeded";

    @Test
    void loggerFailureDoesNotChangeAcquisitionOrGrant() {
        Logger logger = Logger.getLogger(ZLinkApplicationJobQueue.class.getName());
        var attempts = new java.util.concurrent.atomic.AtomicInteger();
        Handler failing =
                new Handler() {
                    public void publish(LogRecord record) {
                        if (record.getMessage().startsWith(IDENTIFIER)) {
                            attempts.incrementAndGet();
                            throw new IllegalStateException("logger failure");
                        }
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(failing);
        try (var queue =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, 1, 1, 1))) {
            var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var pending = new ArrayList<CompletableFuture<ZLinkApplicationJobQueue.Permit>>();
            for (int i = 0; i < 3; i++)
                pending.add(
                        queue.acquire(ZLinkApplicationJobQueue.Origin.LOCAL).toCompletableFuture());
            assertEquals(1, attempts.get());
            assertEquals(3, queue.snapshot().capacityWaiters());
            held.close();
            pending.forEach(wait -> wait.join().close());
            assertEquals(0, queue.snapshot().capacityWaiters());
        } finally {
            logger.removeHandler(failing);
        }
    }

    @Test
    void warnsOnceUntilLocalWaitersReachZero() {
        Logger logger = Logger.getLogger(ZLinkApplicationJobQueue.class.getName());
        List<LogRecord> records = new ArrayList<>();
        Handler capture =
                new Handler() {
                    public void publish(LogRecord record) {
                        if (record.getMessage().startsWith(IDENTIFIER)) records.add(record);
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(capture);
        List<CompletableFuture<ZLinkApplicationJobQueue.Permit>> pending = new ArrayList<>();
        try (var queue =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(3),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, 1, 1, 1))) {
            var held = new ArrayList<ZLinkApplicationJobQueue.Permit>();
            for (int i = 0; i < 3; i++)
                held.add(
                        queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture()
                                .join());
            for (int i = 0; i < 4; i++)
                pending.add(
                        queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture());
            assertEquals(0, records.size());
            for (int episode = 1; episode <= 2; episode++) {
                var local = new ArrayList<CompletableFuture<ZLinkApplicationJobQueue.Permit>>();
                for (int i = 0; i < 3; i++)
                    local.add(
                            queue.acquire(ZLinkApplicationJobQueue.Origin.LOCAL)
                                    .toCompletableFuture());
                pending.addAll(local);
                assertEquals(episode - 1, records.size());
                var excess =
                        queue.acquire(ZLinkApplicationJobQueue.Origin.LOCAL).toCompletableFuture();
                pending.add(excess);
                assertEquals(episode, records.size());
                var record = records.get(records.size() - 1);
                assertEquals(Level.WARNING, record.getLevel());
                assertArrayEquals(new Object[] {4L, 3L}, record.getParameters());
                excess.cancel(false);
                var repeated =
                        queue.acquire(ZLinkApplicationJobQueue.Origin.LOCAL).toCompletableFuture();
                pending.add(repeated);
                assertEquals(episode, records.size());
                queue.resetMetrics();
                repeated.cancel(false);
                local.forEach(wait -> wait.cancel(false));
            }
            pending.forEach(wait -> wait.cancel(false));
            held.forEach(ZLinkApplicationJobQueue.Permit::close);
        } finally {
            logger.removeHandler(capture);
        }
    }
}
