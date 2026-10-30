package systems.zlink.framework.runtime.diagnostics;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkLogLevel;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowResult;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.*;

class ZLinkTelemetryOwnerTest {
    static final class Provider extends Handler implements AutoCloseable {
        final Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        final boolean parents = logger.getUseParentHandlers();
        final Level level = logger.getLevel();
        final CountDownLatch entered = new CountDownLatch(1);
        final CountDownLatch release = new CountDownLatch(1);
        final List<LogRecord> records = new ArrayList<>();
        boolean first = true;

        Provider() {
            ZLinkTelemetryTestFence.awaitIdle();
            logger.setUseParentHandlers(false);
            logger.setLevel(Level.ALL);
            logger.addHandler(this);
        }

        public void publish(LogRecord record) {
            if (first) {
                first = false;
                entered.countDown();
                try {
                    release.await();
                } catch (InterruptedException failure) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(failure);
                }
            }
            records.add(record);
        }

        void awaitEntered() throws Exception {
            assertTrue(entered.await(5, TimeUnit.SECONDS));
        }

        public void flush() {}

        public void close() {
            release.countDown();
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(this);
            logger.setUseParentHandlers(parents);
            logger.setLevel(level);
        }
    }

    static ZLinkDispatchOptionsRegistration options() {
        var options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(ZLinkMessageFlowLogMode.DETAILED);
        options.includeMessageSizes(true);
        return options;
    }

    static ZLinkMessageFlowTracer tracer(
            ZLinkTelemetryOwner.Host host, ZLinkDispatchOptionsRegistration options) {
        return new ZLinkMessageFlowTracer(
                options,
                ZLinkHandlerActivator.services().add(ZLinkTelemetryOwner.Host.class, host),
                command -> {
                    throw new AssertionError("application executor used by telemetry");
                });
    }

    static void emit(ZLinkMessageFlowTracer tracer, int sequence) {
        tracer.trace(
                ZLinkMessageFlowTracerTest.flow(
                        ZLinkMessageFlowOutcome.DISPATCHED, (long) sequence));
    }

    @Test
    void offHostStartAndStopCreateNoWorkerInFreshProcess() throws Exception {
        String classpath = System.getProperty("java.class.path");
        var process =
                new ProcessBuilder(
                                java.nio.file.Path.of(
                                                System.getProperty("java.home"), "bin", "java")
                                        .toString(),
                                "--enable-native-access=ALL-UNNAMED",
                                "-cp",
                                classpath,
                                ZLinkTelemetryOffHostProbe.class.getName())
                        .redirectErrorStream(true)
                        .start();
        try {
            assertTrue(process.waitFor(15, TimeUnit.SECONDS), "Off host probe timed out");
            String output =
                    new String(
                            process.getInputStream().readAllBytes(),
                            java.nio.charset.StandardCharsets.UTF_8);
            assertEquals(0, process.exitValue(), output);
            assertTrue(output.contains("OFF_HOST_TELEMETRY_THREADS=0"), output);
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly();
                process.waitFor();
            }
        }
    }

    @Test
    void exactPendingBoundDropsNewAcrossAllOutcomesAndCoalescesWake() throws Exception {
        try (var provider = new Provider();
                var hostA = ZLinkTelemetryOwner.openHost();
                var hostB = ZLinkTelemetryOwner.openHost()) {
            var a = tracer(hostA, options());
            var b = tracer(hostB, options());
            var before = ZLinkTelemetryOwner.state();
            AtomicLong metric = new AtomicLong();
            AtomicReference<Thread> metricThread = new AtomicReference<>();
            try (var ignored =
                    ZLinkRuntimeMetrics.install(
                            new ZLinkRuntimeMetrics.Sink() {
                                public void add(String name, long delta, Map<String, String> tags) {
                                    if (name.equals(
                                            ZLinkRuntimeMetrics.Metric.OBSERVABILITY_EVENTS_OVERFLOW
                                                    .metricName())) {
                                        assertEquals(Map.of("source", "message_flow"), tags);
                                        metric.addAndGet(delta);
                                        metricThread.set(Thread.currentThread());
                                    }
                                }
                            })) {
                emit(a, -1);
                provider.awaitEntered();
                for (int i = 0; i < ZLinkTelemetryOwner.PENDING_LIMIT; i++)
                    emit(i % 2 == 0 ? a : b, i);
                var full = ZLinkTelemetryOwner.state();
                assertEquals(ZLinkTelemetryOwner.PENDING_LIMIT, full.pending());
                assertTrue(full.executing());
                for (var result :
                        List.of(
                                ZLinkMessageFlowResult.SUCCEEDED,
                                ZLinkMessageFlowResult.FAILED,
                                ZLinkMessageFlowResult.DROPPED)) {
                    a.trace(
                            ZLinkMessageFlowTracerTest.flow(
                                            ZLinkMessageFlowOutcome.DISPATCHED, 99999L)
                                    .withOutcome(result));
                }
                assertEquals(before.overflow() + 3, ZLinkTelemetryOwner.state().overflow());
                assertEquals(before.wakes() + 1, ZLinkTelemetryOwner.state().wakes());
                assertEquals(0, metric.get());
                provider.release.countDown();
                ZLinkTelemetryTestFence.awaitIdle();
                assertEquals(3, metric.get());
                assertEquals("zlink-process-telemetry", metricThread.get().getName());
                assertEquals(ZLinkTelemetryOwner.PENDING_LIMIT + 1, provider.records.size());
                for (int i = 0; i < ZLinkTelemetryOwner.PENDING_LIMIT; i++) {
                    assertTrue(
                            provider.records.get(i + 1).getMessage().contains("size=" + i + " ")
                                    || provider.records
                                            .get(i + 1)
                                            .getMessage()
                                            .endsWith("size=" + i));
                }
            }
        }
    }

    @Test
    void retiringOneHostRemovesOnlyItsPendingWithoutWaitingOrOverflow() throws Exception {
        try (var provider = new Provider();
                var hostA = ZLinkTelemetryOwner.openHost();
                var hostB = ZLinkTelemetryOwner.openHost();
                var hostC = ZLinkTelemetryOwner.openHost()) {
            var a = tracer(hostA, options());
            var b = tracer(hostB, options());
            emit(a, 1);
            provider.awaitEntered();
            emit(a, 2);
            emit(b, 3);
            emit(a, 4);
            long overflow = ZLinkTelemetryOwner.state().overflow();
            hostA.close();
            assertEquals(1, ZLinkTelemetryOwner.state().pending());
            assertTrue(ZLinkTelemetryOwner.state().executing());
            emit(a, 5);
            emit(tracer(hostC, options()), 6);
            assertEquals(2, ZLinkTelemetryOwner.state().pending());
            assertEquals(overflow, ZLinkTelemetryOwner.state().overflow());
            provider.release.countDown();
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(3, provider.records.size());
            assertTrue(provider.records.get(1).getMessage().contains("size=3"));
            assertTrue(provider.records.get(2).getMessage().contains("size=6"));
        }
    }

    @Test
    void commonRuntimeCloseRetiresLeaseBeforeBlockedProviderFinishes() throws Exception {
        var options =
                new systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var target = systems.zlink.contracts.core.RoutingId.from("telemetry-owner");
        options.addRouteMesh("telemetry")
                .listen("inproc://telemetry-owner-" + java.util.UUID.randomUUID())
                .setRoutingId(target);
        var runtime =
                systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess.start(
                        options,
                        new systems.zlink.framework.runtime.binding
                                .ZLinkJavaBackendAdapterFactory());
        Runnable request =
                () ->
                        assertThrows(
                                systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                () ->
                                        runtime.route()
                                                .requestToNode("telemetry", target, "probe")
                                                .timeout(java.time.Duration.ofSeconds(5))
                                                .submit_sync(String.class));
        try (var provider = new Provider()) {
            request.run();
            provider.awaitEntered();
            request.run();
            assertTrue(ZLinkTelemetryOwner.state().pending() > 0);
            var closed = java.util.concurrent.CompletableFuture.runAsync(runtime::close);
            try {
                closed.get(5, TimeUnit.SECONDS);
                assertTrue(ZLinkTelemetryOwner.state().executing());
                assertEquals(0, ZLinkTelemetryOwner.state().pending());
                request.run();
                assertEquals(0, ZLinkTelemetryOwner.state().pending());
            } finally {
                provider.release.countDown();
                closed.get(5, TimeUnit.SECONDS);
            }
        } finally {
            runtime.close();
        }
    }

    @Test
    void snapshotRetainsAdmittedLevelSizeAndObservedTimestamp() throws Exception {
        try (var provider = new Provider();
                var host = ZLinkTelemetryOwner.openHost()) {
            var options = options();
            var tracer = tracer(host, options);
            emit(tracer, 1);
            provider.awaitEntered();
            Instant before = Instant.now();
            emit(tracer, 42);
            Instant after = Instant.now();
            options.messageFlow(ZLinkMessageFlowLogMode.OFF);
            options.includeMessageSizes(false);
            provider.release.countDown();
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(2, provider.records.size());
            var record = provider.records.get(1);
            assertEquals(Level.INFO, record.getLevel());
            assertTrue(record.getMessage().contains("size=42"));
            assertFalse(record.getInstant().isBefore(before));
            assertFalse(record.getInstant().isAfter(after));
        }
    }

    @Test
    void frozenAmbientFlowSeverityAndSanitizedErrorSurviveProducerMutation() throws Exception {
        try (var provider = new Provider();
                var host = ZLinkTelemetryOwner.openHost()) {
            var options = options();
            options.unhandled().setSendLogLevel(ZLinkLogLevel.WARN);
            var factory =
                    ZLinkHandlerActivator.services().add(ZLinkTelemetryOwner.Host.class, host);
            var tracer = tracer(host, options);
            var reporter = new ZLinkDispatchErrorReporter(options, factory, Runnable::run);
            emit(tracer, 1);
            provider.awaitEntered();
            String flowId = "01890f47-5ec8-7000-8000-000000000001";
            var message = new AtomicReference<>("original token=secret\nline");
            var failure =
                    new IllegalStateException() {
                        public String getMessage() {
                            return message.get();
                        }
                    };
            try (var ignored =
                    systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(
                            new systems.zlink.framework.runtime.internal.diagnostics
                                    .ZLinkFlowContext.State(
                                    flowId,
                                    systems.zlink.framework.monitoring.ZLinkFlowOrigin.INBOUND,
                                    "session-original"))) {
                emit(tracer, 42);
                reporter.report(
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchErrorSurface.CHANNEL,
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchMessageKind.SEND,
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchErrorReason.HANDLER_MISSING,
                        systems.zlink.framework.runtime.internal.diagnostics
                                .ZLinkDispatchErrorAction.DROP,
                        "packet",
                        "channel",
                        null,
                        null,
                        null,
                        null,
                        "corr",
                        failure);
            }
            message.set("changed");
            options.unhandled().setSendLogLevel(ZLinkLogLevel.DEBUG);
            options.messageFlow(ZLinkMessageFlowLogMode.OFF);
            provider.release.countDown();
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(3, provider.records.size());
            assertTrue(provider.records.get(1).getMessage().contains("flow=" + flowId));
            assertTrue(provider.records.get(1).getMessage().contains("session=session-original"));
            var error = provider.records.get(2);
            assertEquals(Level.WARNING, error.getLevel());
            assertTrue(error.getMessage().contains("original token=<redacted>"));
            assertFalse(error.getMessage().contains("secret"));
            assertFalse(error.getMessage().contains("changed"));
            assertFalse(error.getMessage().contains("\n"));
            assertNull(
                    systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
                            .current());
        }
    }

    @Test
    void providerFailureAndFailingFallbackAreContainedOnceForProcessLifetime() {
        ZLinkTelemetryTestFence.awaitIdle();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        boolean parents = logger.getUseParentHandlers();
        Level level = logger.getLevel();
        AtomicLong fallbackCalls = new AtomicLong();
        var stderr = System.err;
        Handler broken =
                new Handler() {
                    public void publish(LogRecord record) {
                        throw new IllegalStateException("provider");
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        logger.addHandler(broken);
        System.setErr(
                new java.io.PrintStream(java.io.OutputStream.nullOutputStream()) {
                    public void println(String message) {
                        fallbackCalls.incrementAndGet();
                        throw new IllegalStateException("fallback");
                    }
                });
        var before = ZLinkTelemetryOwner.state();
        try (var a = ZLinkTelemetryOwner.openHost();
                var b = ZLinkTelemetryOwner.openHost()) {
            var tracer = tracer(a, options());
            emit(tracer, 1);
            emit(tracer, 2);
            emit(tracer(b, options()), 3);
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(2, tracer.providerFailureCount());
            assertEquals(before.loggerFailures() + 3, ZLinkTelemetryOwner.state().loggerFailures());
            assertEquals(1, fallbackCalls.get());
            assertEquals(
                    before.fallbackFailures() + 1, ZLinkTelemetryOwner.state().fallbackFailures());
        } finally {
            System.setErr(stderr);
            logger.removeHandler(broken);
            logger.setUseParentHandlers(parents);
            logger.setLevel(level);
        }
    }
}
