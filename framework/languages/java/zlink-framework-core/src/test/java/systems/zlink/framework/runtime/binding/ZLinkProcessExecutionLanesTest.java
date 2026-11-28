package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkProcessExecutionLanesTest {
    @Test
    void slowDiagnosticProviderDoesNotDelayNextApplicationTask() throws Exception {
        var logger = Logger.getLogger(ZLinkProcessExecutionLanes.class.getName());
        var failure = new IllegalStateException("application task failed");
        var delivered = new CompletableFuture<LogRecord>();
        var deliveryThread = new CompletableFuture<String>();
        var providerDelay = Duration.ofMillis(500);
        var handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if (record.getThrown() != failure) return;
                        try {
                            Thread.sleep(providerDelay);
                            deliveryThread.complete(Thread.currentThread().getName());
                            delivered.complete(record);
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            delivered.completeExceptionally(interrupted);
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(handler);
        try {
            var lane = ZLinkProcessExecutionLanes.applicationLane();
            var started = new CompletableFuture<Long>();
            var elapsed = new CompletableFuture<Duration>();
            lane.execute(
                    () -> {
                        started.complete(System.nanoTime());
                        throw failure;
                    });
            lane.execute(
                    () -> elapsed.complete(Duration.ofNanos(System.nanoTime() - started.join())));
            var delay = elapsed.get(5, TimeUnit.SECONDS);
            var record = delivered.get(5, TimeUnit.SECONDS);
            var provider = deliveryThread.get(5, TimeUnit.SECONDS);
            var maximum = Duration.ofMillis(250);
            assertTrue(
                    delay.compareTo(maximum) < 0,
                    () -> "next application task took " + delay + " with provider " + provider);
            assertEquals(Level.SEVERE, record.getLevel());
            assertEquals("application dispatch failed", record.getMessage());
            assertSame(failure, record.getThrown());
            assertEquals("zlink-process-telemetry", provider);
            System.out.println(
                    "next application task took " + delay + " with provider " + provider);
        } finally {
            logger.removeHandler(handler);
        }
    }

    @Test
    void topologiesShareInfrastructureWithoutBlockingEachOthersApplicationLane() throws Exception {
        assertSame(ZLinkProcessExecutionLanes.deadlines(), ZLinkProcessExecutionLanes.deadlines());
        var blockedTopology = ZLinkProcessExecutionLanes.applicationLane();
        var progressingTopology = ZLinkProcessExecutionLanes.applicationLane();
        assertNotSame(blockedTopology, progressingTopology);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletableFuture<Void> blockedStarted = new CompletableFuture<>();
        CompletableFuture<Void> progressed = new CompletableFuture<>();

        blockedTopology.execute(
                () -> {
                    blockedStarted.complete(null);
                    release.join();
                });
        blockedStarted.get(3, TimeUnit.SECONDS);
        progressingTopology.execute(() -> progressed.complete(null));

        progressed.get(3, TimeUnit.SECONDS);
        release.complete(null);
    }
}
