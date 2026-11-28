package systems.zlink.framework.runtime.diagnostics;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;

final class ZLinkTelemetryQueueTest {
    private static final String WORKER_NAME = "zlink-process-telemetry";

    @Test
    void concurrentProducersPreserveOrderAndAccountForEverySubmission() throws Exception {
        int producers = 8;
        int recordsPerProducer = 1536;
        int submitted = producers * recordsPerProducer;
        var logger = Logger.getLogger(ZLinkTelemetryQueueTest.class.getName());
        var dropped = new AtomicLong();
        try (var provider = new ZLinkTelemetryOwnerTest.Provider(logger);
                var host = ZLinkTelemetryOwner.openHost();
                var metrics =
                        ZLinkRuntimeMetrics.install(
                                new ZLinkRuntimeMetrics.Sink() {
                                    @Override
                                    public void add(
                                            String name, long delta, Map<String, String> tags) {
                                        if (name.equals("zlink.observability.events.overflow")
                                                && tags.equals(Map.of("source", "diagnostic"))) {
                                            dropped.addAndGet(delta);
                                        }
                                    }
                                })) {
            long overflowBefore = ZLinkTelemetryOwner.state().overflow();
            ZLinkTelemetryOwner.log(host, logger, Level.WARNING, () -> "gate", null);
            provider.awaitEntered();
            try {
                runTogether(
                        producers,
                        producer -> {
                            for (int i = 0; i < recordsPerProducer; i++) {
                                int id = producer * recordsPerProducer + i;
                                ZLinkTelemetryOwner.log(
                                        host,
                                        logger,
                                        Level.WARNING,
                                        () -> Integer.toString(id),
                                        null);
                            }
                        });
                assertEquals(
                        ZLinkTelemetryOwner.PENDING_LIMIT, ZLinkTelemetryOwner.state().pending());
                assertEquals(
                        submitted - ZLinkTelemetryOwner.PENDING_LIMIT,
                        ZLinkTelemetryOwner.state().overflow() - overflowBefore);
            } finally {
                provider.release.countDown();
            }
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(submitted - ZLinkTelemetryOwner.PENDING_LIMIT, dropped.get());
            var delivered = provider.records.subList(1, provider.records.size());
            assertEquals(submitted - dropped.get(), delivered.size());

            var previous = new ArrayList<>(Collections.nCopies(producers, -1));
            for (var record : delivered) {
                int id = Integer.parseInt(record.getMessage());
                assertTrue(id >= 0 && id < submitted, "unknown record " + id);
                int producer = id / recordsPerProducer;
                int sequence = id % recordsPerProducer;
                assertTrue(
                        sequence > previous.set(producer, sequence),
                        "out-of-order or duplicate record " + id);
            }
        }
    }

    @Test
    void concurrentPrepareAndFirstSubmissionStartOneWorker() throws Exception {
        var process =
                new ProcessBuilder(
                                Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                                "-cp",
                                System.getProperty("java.class.path"),
                                ZLinkTelemetryQueueTest.class.getName())
                        .redirectErrorStream(true)
                        .start();
        try {
            assertTrue(process.waitFor(15, TimeUnit.SECONDS), "prepare probe timed out");
            String output =
                    new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            assertEquals(0, process.exitValue(), output);
            assertTrue(output.contains("TELEMETRY_WORKERS=1"), output);
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly();
                process.waitFor();
            }
        }
    }

    /** Runs in a fresh JVM so the two submissions contend for the first worker. */
    public static void main(String[] args) throws Exception {
        assertEquals(0, workerCount());
        var logger = Logger.getLogger(ZLinkTelemetryQueueTest.class.getName());
        try (var provider = new ZLinkTelemetryOwnerTest.Provider(logger);
                var host = ZLinkTelemetryOwner.openHost()) {
            runTogether(
                    2,
                    producer -> {
                        ZLinkTelemetryOwner.prepare();
                        ZLinkTelemetryOwner.log(
                                host, logger, Level.WARNING, () -> "first-" + producer, null);
                    });
            provider.awaitEntered();
            provider.release.countDown();
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(
                    java.util.List.of("first-0", "first-1"),
                    provider.records.stream().map(record -> record.getMessage()).sorted().toList());
            long workers = workerCount();
            assertEquals(1, workers);
            System.out.println("TELEMETRY_WORKERS=" + workers);
        }
    }

    private static long workerCount() {
        return Thread.getAllStackTraces().keySet().stream()
                .filter(thread -> thread.getName().equals(WORKER_NAME))
                .count();
    }

    private static void runTogether(int count, java.util.function.IntConsumer action)
            throws Exception {
        var ready = new CountDownLatch(count);
        var start = new CountDownLatch(1);
        try (var executor = Executors.newFixedThreadPool(count)) {
            var tasks = new ArrayList<Future<Void>>(count);
            try {
                for (int i = 0; i < count; i++) {
                    int producer = i;
                    tasks.add(
                            executor.submit(
                                    () -> {
                                        ready.countDown();
                                        if (!start.await(5, TimeUnit.SECONDS)) {
                                            throw new AssertionError(
                                                    "producer did not receive the start signal");
                                        }
                                        action.accept(producer);
                                        return null;
                                    }));
                }
                assertTrue(ready.await(5, TimeUnit.SECONDS), "producers did not reach the barrier");
            } finally {
                start.countDown();
            }
            for (var task : tasks) task.get(5, TimeUnit.SECONDS);
        }
    }
}
