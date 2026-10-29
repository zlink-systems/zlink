package systems.zlink.framework.runtime.diagnostics;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/** Fresh processes exercise each role's lifetime without resetting the production singleton. */
class ZLinkTelemetryProviderFailureTest {
    @Test
    void traceFailurePrecedesLoggerFailureAndRolesReportIndependently() throws Exception {
        fork(false);
    }

    @Test
    void failedTraceFallbackIsNotReusedForFirstLoggerFailure() throws Exception {
        fork(true);
    }

    private static void fork(boolean failFirstFallback) throws Exception {
        var child =
                new ProcessBuilder(
                                java.nio.file.Path.of(
                                                System.getProperty("java.home"), "bin", "java")
                                        .toString(),
                                "--enable-native-access=ALL-UNNAMED",
                                "-cp",
                                System.getProperty("java.class.path"),
                                ZLinkTelemetryProviderFailureTest.class.getName(),
                                Boolean.toString(failFirstFallback))
                        .redirectErrorStream(true)
                        .start();
        try {
            assertTrue(child.waitFor(20, TimeUnit.SECONDS), "role probe timed out");
            String output =
                    new String(
                            child.getInputStream().readAllBytes(),
                            java.nio.charset.StandardCharsets.UTF_8);
            assertEquals(0, child.exitValue(), output);
            assertTrue(output.contains("ROLE_FAILURES_CONTAINED"), output);
        } finally {
            if (child.isAlive()) {
                child.destroyForcibly();
                child.waitFor();
            }
        }
    }

    public static void main(String[] args) throws Exception {
        boolean failFirstFallback = Boolean.parseBoolean(args[0]);
        Logger flowLogger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Logger metricLogger = Logger.getLogger(ZLinkRuntimeMetrics.class.getName());
        flowLogger.setUseParentHandlers(false);
        flowLogger.setLevel(Level.ALL);
        metricLogger.setUseParentHandlers(false);
        metricLogger.setLevel(Level.ALL);
        var loggerGate = new AtomicReference<Gate>();
        var metricGate = new AtomicReference<Gate>();
        AtomicLong metricCalls = new AtomicLong();
        AtomicLong unexpectedMetricCallbacks = new AtomicLong();
        AtomicLong fallbackCalls = new AtomicLong();
        AtomicReference<Thread> metricThread = new AtomicReference<>();
        AtomicReference<Thread> fallbackThread = new AtomicReference<>();
        var loggerFailure = new AtomicReference<Throwable>();
        List<String> fallbackMessages = new ArrayList<>();
        Handler primary =
                new Handler() {
                    public void publish(LogRecord record) {
                        Gate gate = loggerGate.getAndSet(null);
                        if (gate != null) gate.block();
                        if (loggerFailure.get() != null)
                            throw new IllegalStateException("logger provider");
                    }

                    public void flush() {}

                    public void close() {}
                };
        Handler observer =
                new Handler() {
                    public void publish(LogRecord record) {
                        unexpectedMetricCallbacks.incrementAndGet();
                    }

                    public void flush() {}

                    public void close() {}
                };
        flowLogger.addHandler(primary);
        metricLogger.addHandler(observer);
        var stderr = System.err;
        System.setErr(
                new java.io.PrintStream(java.io.OutputStream.nullOutputStream()) {
                    public void println(String message) {
                        fallbackThread.set(Thread.currentThread());
                        fallbackMessages.add(message);
                        long count = fallbackCalls.incrementAndGet();
                        if (failFirstFallback || count == 2)
                            throw new IllegalStateException("fallback provider");
                    }
                });
        try (var host = ZLinkTelemetryOwner.openHost();
                var registration =
                        ZLinkRuntimeMetrics.install(
                                new ZLinkRuntimeMetrics.Sink() {
                                    public void add(
                                            String name, long delta, Map<String, String> tags) {
                                        assertEquals(
                                                ZLinkRuntimeMetrics.Metric
                                                        .OBSERVABILITY_EVENTS_OVERFLOW
                                                        .metricName(),
                                                name);
                                        assertEquals(Map.of("source", "message_flow"), tags);
                                        assertEquals(1, delta);
                                        metricCalls.incrementAndGet();
                                        metricThread.set(Thread.currentThread());
                                        Gate gate = metricGate.getAndSet(null);
                                        assertNotNull(gate);
                                        gate.block();
                                        throw new IllegalStateException("trace provider");
                                    }
                                })) {
            var tracer = ZLinkTelemetryOwnerTest.tracer(host, ZLinkTelemetryOwnerTest.options());
            overflow(tracer, loggerGate, metricGate);
            assertEquals(
                    0,
                    unexpectedMetricCallbacks.get(),
                    "overflow failure must bypass general metric observer logger");
            assertEquals(1, traceFailures());
            assertEquals(0, ZLinkTelemetryOwner.state().loggerFailures());
            assertEquals(1, fallbackCalls.get());
            assertTrue(fallbackMessages.getFirst().contains("trace"));
            assertEquals("zlink-process-telemetry", metricThread.get().getName());
            assertSame(metricThread.get(), fallbackThread.get());

            overflow(tracer, loggerGate, metricGate);
            assertEquals(2, traceFailures());
            assertEquals(1, fallbackCalls.get(), "same role must report only its first failure");
            assertEquals(0, ZLinkTelemetryOwner.state().loggerFailures());
            loggerFailure.set(new IllegalStateException("enabled"));
            ZLinkTelemetryOwnerTest.emit(tracer, 7);
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(1, tracer.providerFailureCount());
            assertEquals(1, ZLinkTelemetryOwner.state().loggerFailures());
            assertEquals(failFirstFallback ? 1 : 2, fallbackCalls.get());
            if (!failFirstFallback) assertTrue(fallbackMessages.get(1).contains("logger"));
            assertEquals(1, ZLinkTelemetryOwner.state().fallbackFailures());
            ZLinkTelemetryOwnerTest.emit(tracer, 8);
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(2, tracer.providerFailureCount());
            assertEquals(2, ZLinkTelemetryOwner.state().loggerFailures());
            assertEquals(2, traceFailures());
            assertEquals(2, metricCalls.get());
            assertEquals(0, unexpectedMetricCallbacks.get());
            assertEquals(
                    failFirstFallback ? 1 : 2,
                    fallbackCalls.get(),
                    "failed fallback must never be reused");
        } finally {
            System.setErr(stderr);
            flowLogger.removeHandler(primary);
            metricLogger.removeHandler(observer);
        }
        System.out.println("ROLE_FAILURES_CONTAINED");
    }

    private static long traceFailures() throws Exception {
        var state = ZLinkTelemetryOwner.state();
        return (Long) state.getClass().getDeclaredMethod("traceFailures").invoke(state);
    }

    private static void overflow(
            ZLinkMessageFlowTracer tracer,
            AtomicReference<Gate> loggerGate,
            AtomicReference<Gate> metricGate)
            throws Exception {
        Gate provider = new Gate();
        Gate metric = new Gate();
        loggerGate.set(provider);
        metricGate.set(metric);
        ZLinkTelemetryOwnerTest.emit(tracer, -1);
        try {
            provider.awaitEntry();
            for (int i = 0; i < ZLinkTelemetryOwner.PENDING_LIMIT; i++)
                ZLinkTelemetryOwnerTest.emit(tracer, i);
            ZLinkTelemetryOwnerTest.emit(tracer, 99999);
            provider.release.countDown();
            metric.awaitEntry();
            assertTrue(
                    ZLinkTelemetryOwner.state().executing(), "overflow provider executes on owner");
        } finally {
            provider.release.countDown();
            metric.release.countDown();
        }
        ZLinkTelemetryTestFence.awaitIdle();
    }

    private static final class Gate {
        final CountDownLatch entered = new CountDownLatch(1);
        final CountDownLatch release = new CountDownLatch(1);

        void awaitEntry() throws Exception {
            assertTrue(entered.await(5, TimeUnit.SECONDS));
        }

        void block() {
            entered.countDown();
            try {
                release.await();
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                throw new AssertionError(failure);
            }
        }
    }
}
