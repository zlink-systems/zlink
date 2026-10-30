package systems.zlink.framework.runtime.diagnostics;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkActivationState;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkChannelRouteKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowResult;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkTraceEventId;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.testutil.PerCallAllocation;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

// MFLOW-001/002/003/011: mode gating + runtime live toggle + structured output.
class ZLinkMessageFlowTracerTest {
    @org.junit.jupiter.api.BeforeEach
    void finishPriorProviderDeliveriesBeforeInstallingTestHandlers() {
        ZLinkTelemetryTestFence.awaitIdle();
    }

    @Test
    void blockedLoggerDoesNotBlockProducer() throws Exception {
        blockedLoggerProducer(false);
    }

    @Test
    void blockedLoggerRunsOnDedicatedThread() throws Exception {
        blockedLoggerProducer(true);
    }

    private static void blockedLoggerProducer(boolean checkThread) throws Exception {
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        var entered = new java.util.concurrent.CountDownLatch(1);
        var release = new java.util.concurrent.CountDownLatch(1);
        var delivered = new AtomicReference<Thread>();
        Handler handler =
                new Handler() {
                    public void publish(LogRecord record) {
                        delivered.set(Thread.currentThread());
                        entered.countDown();
                        try {
                            release.await();
                        } catch (InterruptedException ex) {
                            Thread.currentThread().interrupt();
                        }
                    }

                    public void flush() {}

                    public void close() {}
                };
        boolean parents = logger.getUseParentHandlers();
        Level level = logger.getLevel();
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        logger.addHandler(handler);
        var tracer = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        var returned = new java.util.concurrent.CountDownLatch(1);
        Thread producer =
                new Thread(
                        () -> {
                            tracer.trace(flow(ZLinkMessageFlowOutcome.DISPATCHED));
                            returned.countDown();
                        },
                        "message-operation-producer");
        try {
            producer.start();
            assertTrue(entered.await(5, java.util.concurrent.TimeUnit.SECONDS));
            if (checkThread) {
                assertTrue(delivered.get() != producer, "provider must use dedicated worker");
            } else {
                assertTrue(
                        returned.await(1, java.util.concurrent.TimeUnit.SECONDS),
                        "producer must return while provider remains blocked");
            }
        } finally {
            release.countDown();
            producer.join(5000);
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(handler);
            logger.setUseParentHandlers(parents);
            logger.setLevel(level);
        }
    }

    @Test
    void channelTraceGateAllocatesNothingWhenOff() {
        var off = tracer(options(ZLinkMessageFlowLogMode.OFF));
        assertEquals(
                0L,
                PerCallAllocation.minWindow(
                        () -> off.begin(ZLinkMessageFlowOutcome.DISPATCHED), 10000, 10000));

        var normal = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        assertTrue(
                PerCallAllocation.minWindow(
                                () -> normal.begin(ZLinkMessageFlowOutcome.DISPATCHED),
                                10000,
                                10000)
                        > 0L);
    }

    @Test
    void substituteProviderPredicateUsesTheExistingDelegateWithoutAllocating() {
        var logger =
                new org.slf4j.helpers.SubstituteLogger(
                        "provider-bootstrap", new java.util.ArrayDeque<>(), false);
        assertFalse(
                ZLinkTelemetryOwner.hasProvider(logger),
                "an unresolved bootstrap logger has no provider");

        logger.setDelegate(org.slf4j.helpers.NOPLogger.NOP_LOGGER);
        assertFalse(
                ZLinkTelemetryOwner.hasProvider(logger),
                "a bootstrap logger bound to NOP has no provider");
        assertEquals(
                0L,
                PerCallAllocation.minWindow(
                        () -> ZLinkTelemetryOwner.hasProvider(logger), 10000, 10000),
                "querying a NOP delegate must allocate zero bytes");

        logger.setDelegate(org.slf4j.LoggerFactory.getLogger(ZLinkMessageFlowTracer.class));
        assertTrue(
                ZLinkTelemetryOwner.hasProvider(logger),
                "a bootstrap logger bound to the configured test provider must be accepted");
        assertEquals(
                0L,
                PerCallAllocation.minWindow(
                        () -> ZLinkTelemetryOwner.hasProvider(logger), 10000, 10000),
                "querying a configured delegate must allocate zero bytes");
    }

    @Test
    void configuredProviderPredicateAllocatesNothing() {
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Handler handler = new java.util.logging.StreamHandler();
        logger.addHandler(handler);
        try {
            var detailed = tracer(options(ZLinkMessageFlowLogMode.DETAILED));
            assertTrue(detailed.captureEnabled());
            long allocated = PerCallAllocation.minWindow(detailed::captureEnabled, 10000, 10000);
            assertEquals(0L, allocated, "provider presence query must allocate zero bytes");
        } finally {
            logger.removeHandler(handler);
            handler.close();
        }
    }

    @Test
    void noProviderDetailedSkipsCaptureAndBuildersWithoutAllocation() throws Exception {
        String service = "META-INF/services/org.slf4j.spi.SLF4JServiceProvider";
        String classpath =
                Arrays.stream(
                                System.getProperty("java.class.path")
                                        .split(
                                                java.util.regex.Pattern.quote(
                                                        java.io.File.pathSeparator)))
                        .filter(
                                entry ->
                                        !java.nio.file.Files.isRegularFile(
                                                java.nio.file.Path.of(entry, service)))
                        .collect(java.util.stream.Collectors.joining(java.io.File.pathSeparator));
        var child =
                new ProcessBuilder(
                                java.nio.file.Path.of(
                                                System.getProperty("java.home"), "bin", "java")
                                        .toString(),
                                "--enable-native-access=ALL-UNNAMED",
                                "-cp",
                                classpath,
                                ZLinkMessageFlowTracerTest.class.getName())
                        .redirectErrorStream(true)
                        .start();
        try {
            assertTrue(
                    child.waitFor(20, java.util.concurrent.TimeUnit.SECONDS),
                    "provider absence probe timed out");
            String output =
                    new String(
                            child.getInputStream().readAllBytes(),
                            java.nio.charset.StandardCharsets.UTF_8);
            assertEquals(0, child.exitValue(), output);
            assertTrue(output.contains("DEFAULT_JVM_NO_PROVIDER_CAPTURE_ZERO"), output);
        } finally {
            if (child.isAlive()) {
                child.destroyForcibly();
                child.waitFor();
            }
        }
    }

    public static void main(String[] args) {
        assertNull(System.getProperty("java.util.logging.config.file"));
        assertNull(System.getProperty("java.util.logging.config.class"));
        assertTrue(
                Arrays.stream(Logger.getLogger("").getHandlers())
                        .anyMatch(java.util.logging.ConsoleHandler.class::isInstance),
                "the default JUL ConsoleHandler must remain installed");
        assertTrue(
                org.slf4j.LoggerFactory.getLogger(ZLinkMessageFlowTracer.class)
                        instanceof org.slf4j.helpers.NOPLogger,
                "the fresh JVM must have no configured SLF4J provider");
        var detailed = tracer(options(ZLinkMessageFlowLogMode.DETAILED));
        int[] capturesAndBuilders = new int[2];
        java.util.function.Supplier<Object> operation =
                () -> {
                    if (detailed.captureEnabled()) capturesAndBuilders[0]++;
                    var point = detailed.begin(ZLinkMessageFlowOutcome.DISPATCHED);
                    if (point != null) capturesAndBuilders[1]++;
                    var error = detailed.beginDispatchError();
                    if (error != null) capturesAndBuilders[1]++;
                    var terminal = detailed.beginRequestTerminal(null, null);
                    if (terminal != null) capturesAndBuilders[1]++;
                    return point;
                };
        long allocated = PerCallAllocation.minWindow(operation, 10000, 10000);
        org.junit.jupiter.api.Assertions.assertAll(
                () ->
                        assertEquals(
                                0,
                                capturesAndBuilders[0],
                                "capture must not run without a provider"),
                () ->
                        assertEquals(
                                0,
                                capturesAndBuilders[1],
                                "builders must not run without a provider"),
                () ->
                        assertEquals(
                                0L,
                                allocated,
                                "provider-free processing must allocate zero bytes"));
        System.out.println("DEFAULT_JVM_NO_PROVIDER_CAPTURE_ZERO");
    }

    private static void awaitRecords(List<String> records, int count) {
        long end = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
        synchronized (records) {
            while (records.size() < count) {
                long remaining = end - System.nanoTime();
                if (remaining <= 0) throw new AssertionError("provider receipt missing");
                try {
                    java.util.concurrent.TimeUnit.NANOSECONDS.timedWait(records, remaining);
                } catch (InterruptedException failure) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(failure);
                }
            }
        }
    }

    private static ZLinkDispatchOptionsRegistration options(ZLinkMessageFlowLogMode mode) {
        ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(mode);
        return options;
    }

    private static ZLinkMessageFlowTracer tracer(ZLinkDispatchOptionsRegistration options) {
        return new ZLinkMessageFlowTracer(
                options, ZLinkHandlerActivator.reflection(), Runnable::run);
    }

    static ZLinkMessageFlowEvent flow(ZLinkMessageFlowOutcome phase) {
        return flow(phase, null);
    }

    static ZLinkMessageFlowEvent flow(ZLinkMessageFlowOutcome phase, Long messageSize) {
        return new ZLinkMessageFlowEvent(
                phase,
                ZLinkDispatchErrorSurface.CHANNEL,
                ZLinkDispatchMessageKind.REQUEST,
                "PlaceOrder",
                "orders",
                null,
                "corr-1",
                null,
                null,
                null,
                messageSize);
    }

    @Test
    void offSuppressesAllTransitions() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.OFF));
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.DROPPED));
    }

    @Test
    void offDoesNotInspectActivationFailureDetails() {
        var tracer = tracer(options(ZLinkMessageFlowLogMode.OFF));
        var failure =
                new RuntimeException() {
                    @Override
                    public String getMessage() {
                        throw new AssertionError("Off must not format activation failures");
                    }
                };
        tracer.traceInstanceActivationDrop(
                "room", null, failure, ZLinkDispatchErrorReason.ACTIVATION_REJECTED, null);
        assertEquals(0, tracer.tracedCount());
    }

    @Test
    void flowCaptureIsOnOnlyAtNormalAndDetailed() {
        //  04-flow-correlation.ko.md:91 (§4): message-flow tracing is on at Normal and
        //  Detailed; Off and Errors skip flow creation/installation/propagation.
        //  03-message-flow-tracing.ko.md:201: Errors still records dispatch
        //  errors, backpressure and drops.
        assertFalse(tracer(options(ZLinkMessageFlowLogMode.OFF)).captureEnabled());
        assertFalse(tracer(options(ZLinkMessageFlowLogMode.ERRORS)).captureEnabled());
        assertTrue(tracer(options(ZLinkMessageFlowLogMode.NORMAL)).captureEnabled());
        assertTrue(tracer(options(ZLinkMessageFlowLogMode.DETAILED)).captureEnabled());
        assertFalse(tracer(options(ZLinkMessageFlowLogMode.OFF)).dispatchErrorsEnabled());
        assertTrue(tracer(options(ZLinkMessageFlowLogMode.ERRORS)).dispatchErrorsEnabled());
    }

    @Test
    void errorsDirectAndCapturedPointsKeepErrorsAndCorrelationWithoutFlowOrAmbientSession() {
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        List<String> records = new ArrayList<>();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        synchronized (records) {
                            records.add(record.getMessage());
                            records.notifyAll();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        boolean useParentHandlers = logger.getUseParentHandlers();
        Level previousLevel = logger.getLevel();
        logger.addHandler(handler);
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        var outer = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        var ambient =
                new ZLinkFlowContext.State(
                        "01890f47-5ec8-7000-8000-000000000001",
                        ZLinkFlowOrigin.INBOUND,
                        "foreign-session");
        assertTrue(outer.captureEnabled());
        try (var ignored = ZLinkFlowContext.enter(ambient)) {
            for (boolean dispatchError : new boolean[] {true, false}) {
                for (boolean direct : new boolean[] {true, false}) {
                    for (boolean explicitFlow : new boolean[] {true, false}) {
                        for (boolean explicitSession : new boolean[] {true, false}) {
                            ZLinkTelemetryTestFence.awaitIdle();
                            records.clear();
                            var options = options(ZLinkMessageFlowLogMode.NORMAL);
                            options.traceSampleRate(0.0d);
                            var live = new AtomicReference<>(ZLinkMessageFlowLogMode.NORMAL);
                            options.diagnostics().installLiveMode(live);
                            var tracer = tracer(options);
                            var event =
                                    dispatchError
                                            ? error(
                                                    ZLinkDispatchMessageKind.REQUEST,
                                                    ZLinkDispatchErrorReason.HANDLER_EXCEPTION)
                                            : flow(ZLinkMessageFlowOutcome.DROPPED);
                            if (explicitFlow) {
                                event =
                                        event.withFlow(
                                                "01890f47-5ec8-7000-8000-000000000002",
                                                ZLinkFlowOrigin.APPLICATION);
                            }
                            if (explicitSession) {
                                event = event.withStreamSessionId("message-session");
                            }
                            live.set(ZLinkMessageFlowLogMode.ERRORS);
                            if (direct) {
                                tracer.trace(event);
                            } else {
                                var point =
                                        dispatchError
                                                ? tracer.beginDispatchError()
                                                : tracer.begin(ZLinkMessageFlowOutcome.DROPPED);
                                assertTrue(point != null);
                                point.trace(event);
                            }
                            assertEquals(1L, tracer.tracedCount());
                            awaitRecords(records, 1);
                            assertEquals(1, records.size());
                            String record = records.getFirst();
                            assertTrue(record.contains(" corr=corr-1"), record);
                            assertTrue(record.contains(" packet=PlaceOrder"), record);
                            assertTrue(record.contains(" channel=orders"), record);
                            assertTrue(
                                    record.contains(
                                            dispatchError
                                                    ? "event_id=zlink.dispatch_error"
                                                    : " phase=dropped"),
                                    record);
                            assertFalse(record.contains(" flow="), record);
                            assertFalse(record.contains(" origin="), record);
                            assertFalse(record.contains("foreign-session"), record);
                            assertEquals(
                                    explicitSession, record.contains(" session=message-session"));
                            assertEquals(0L, tracer.providerFailureCount());
                        }
                    }
                }
            }
        } finally {
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(handler);
            logger.setUseParentHandlers(useParentHandlers);
            logger.setLevel(previousLevel);
        }
    }

    @Test
    void errorsReporterInsideNormalFlowKeepsOneUnsampledErrorWithoutBorrowingFlow() {
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        List<String> records = new ArrayList<>();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        synchronized (records) {
                            records.add(record.getMessage());
                            records.notifyAll();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        boolean useParentHandlers = logger.getUseParentHandlers();
        Level previousLevel = logger.getLevel();
        logger.addHandler(handler);
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        var outer = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        assertTrue(outer.captureEnabled());
        var ambient =
                new ZLinkFlowContext.State(
                        "01890f47-5ec8-7000-8000-000000000001",
                        ZLinkFlowOrigin.INBOUND,
                        "foreign-session");
        try (var ignored = ZLinkFlowContext.enter(ambient)) {
            var options = options(ZLinkMessageFlowLogMode.ERRORS);
            options.traceSampleRate(0.0d);
            var reporter =
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run);
            reporter.report(
                    ZLinkDispatchErrorSurface.CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                    ZLinkDispatchErrorAction.REPLY_ERROR,
                    "PlaceOrder",
                    "orders",
                    null,
                    null,
                    null,
                    null,
                    "corr-1",
                    new IllegalStateException("handler failed"));

            assertEquals(1L, reporter.reportedCount());
            assertEquals(1L, reporter.flow().tracedCount());
            awaitRecords(records, 1);
            assertEquals(1, records.size());
            String record = records.getFirst();
            assertTrue(record.contains("event_id=zlink.dispatch_error"), record);
            assertTrue(record.contains(" corr=corr-1"), record);
            assertTrue(record.contains(" reason=handler_exception"), record);
            assertTrue(record.contains(" action=reply_error"), record);
            assertTrue(record.contains(" error_type=IllegalStateException"), record);
            assertTrue(record.contains(" error_message=handler failed"), record);
            assertFalse(record.contains(" flow="), record);
            assertFalse(record.contains(" origin="), record);
            assertFalse(record.contains(" session="), record);
            assertEquals(0L, reporter.flow().providerFailureCount());
        } finally {
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(handler);
            logger.setUseParentHandlers(useParentHandlers);
            logger.setLevel(previousLevel);
        }
    }

    @Test
    void normalPointPreservesExplicitFlowOrAmbientFallbackAfterLiveModeChanges() {
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        List<String> records = new ArrayList<>();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        synchronized (records) {
                            records.add(record.getMessage());
                            records.notifyAll();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        boolean useParentHandlers = logger.getUseParentHandlers();
        Level previousLevel = logger.getLevel();
        logger.addHandler(handler);
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        var ambient =
                new ZLinkFlowContext.State(
                        "01890f47-5ec8-7000-8000-000000000001",
                        ZLinkFlowOrigin.INBOUND,
                        "ambient-session");
        try (var ignored = ZLinkFlowContext.enter(ambient)) {
            for (boolean explicitFlow : new boolean[] {true, false}) {
                for (boolean switchAfterBegin : new boolean[] {true, false}) {
                    ZLinkTelemetryTestFence.awaitIdle();
                    records.clear();
                    var options = options(ZLinkMessageFlowLogMode.NORMAL);
                    var live = new AtomicReference<>(ZLinkMessageFlowLogMode.NORMAL);
                    options.diagnostics().installLiveMode(live);
                    var tracer = tracer(options);
                    var event = flow(ZLinkMessageFlowOutcome.RECEIVED);
                    if (explicitFlow) {
                        event =
                                event.withFlow(
                                                "01890f47-5ec8-7000-8000-000000000002",
                                                ZLinkFlowOrigin.APPLICATION)
                                        .withStreamSessionId("message-session");
                    }
                    var point = tracer.begin(ZLinkMessageFlowOutcome.RECEIVED);
                    assertTrue(point != null);
                    if (switchAfterBegin) {
                        live.set(ZLinkMessageFlowLogMode.ERRORS);
                    }
                    point.trace(event);

                    assertEquals(1L, tracer.tracedCount());
                    awaitRecords(records, 1);
                    assertEquals(1, records.size());
                    String record = records.getFirst();
                    assertTrue(record.contains(" corr=corr-1"), record);
                    assertTrue(
                            record.contains(
                                    " flow="
                                            + (explicitFlow
                                                    ? "01890f47-5ec8-7000-8000-000000000002"
                                                    : ambient.flowId())),
                            record);
                    assertTrue(
                            record.contains(
                                    " origin=" + (explicitFlow ? "application" : "inbound")),
                            record);
                    assertTrue(
                            record.contains(
                                    " session="
                                            + (explicitFlow
                                                    ? "message-session"
                                                    : "ambient-session")),
                            record);
                    assertEquals(0L, tracer.providerFailureCount());
                }
            }
        } finally {
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(handler);
            logger.setUseParentHandlers(useParentHandlers);
            logger.setLevel(previousLevel);
        }
    }

    @Test
    void eventStillRejectsEitherHalfOfAFlowPair() {
        var event = flow(ZLinkMessageFlowOutcome.DROPPED);
        assertThrows(
                IllegalArgumentException.class,
                () -> event.withFlow("01890f47-5ec8-7000-8000-000000000002", null));
        assertThrows(
                IllegalArgumentException.class,
                () -> event.withFlow(null, ZLinkFlowOrigin.APPLICATION));
    }

    @Test
    void errorsOnlyEmitsDroppedNotReceived() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.ERRORS));
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.DROPPED));
    }

    @Test
    void omittedMessageSizeSettingIsOffAtDetailedLevel() {
        ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
        assertFalse(options.diagnostics().includeMessageSizes());
        options.messageFlow(ZLinkMessageFlowLogMode.DETAILED);

        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        List<String> records = new ArrayList<>();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        synchronized (records) {
                            records.add(record.getMessage());
                            records.notifyAll();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        boolean useParentHandlers = logger.getUseParentHandlers();
        Level level = logger.getLevel();
        logger.addHandler(handler);
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        try {
            tracer(options).trace(flow(ZLinkMessageFlowOutcome.RECEIVED, 42L));

            awaitRecords(records, 1);
            assertEquals(1, records.size());
            assertFalse(records.getFirst().contains("message_size_bytes="));
            assertFalse(records.getFirst().contains("size=42"));
        } finally {
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(handler);
            logger.setUseParentHandlers(useParentHandlers);
            logger.setLevel(level);
        }
    }

    @Test
    void keyTransitionsEmitsLifecycle() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.REPLIED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.ADMITTED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.COMPLETED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.BACKPRESSURED));
    }

    @Test
    void liveModeOverridesAndTogglesAtRuntime() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.OFF);
        AtomicReference<ZLinkMessageFlowLogMode> cell =
                new AtomicReference<>(ZLinkMessageFlowLogMode.OFF);
        options.diagnostics().installLiveMode(cell);
        ZLinkMessageFlowTracer tracer = tracer(options);

        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        cell.set(ZLinkMessageFlowLogMode.NORMAL);
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        cell.set(ZLinkMessageFlowLogMode.OFF);
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
    }

    @Test
    void tracesStructuredTransitions() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.NORMAL);
        ZLinkMessageFlowTracer tracer = tracer(options);

        tracer.trace(flow(ZLinkMessageFlowOutcome.RECEIVED));
        tracer.trace(flow(ZLinkMessageFlowOutcome.REPLIED));

        assertEquals(2L, tracer.tracedCount());
    }

    @Test
    void processingPointUsesTheSingleLiveLevelReadCapturedByBegin() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.NORMAL);
        ZLinkMessageFlowTracer tracer = tracer(options);

        ZLinkMessageFlowTracer.TracePoint point = tracer.begin(ZLinkMessageFlowOutcome.RECEIVED);
        options.messageFlow(ZLinkMessageFlowLogMode.OFF);
        point.trace(flow(ZLinkMessageFlowOutcome.RECEIVED));

        assertEquals(1L, tracer.tracedCount());
    }

    @Test
    void spec26PhasesAndOutcomesAreSeparateClosedVocabularies() {
        assertEquals(
                Set.of(
                        "RECEIVED",
                        "ADMITTED",
                        "DISPATCHED",
                        "COMPLETED",
                        "REPLIED",
                        "SENT",
                        "REPLY_RECEIVED",
                        "BACKPRESSURED",
                        "DROPPED"),
                Set.copyOf(
                        Arrays.stream(ZLinkMessageFlowOutcome.values()).map(Enum::name).toList()));
        assertEquals(
                Set.of("succeeded", "failed", "backpressured", "dropped", "cancelled", "shutdown"),
                Set.copyOf(
                        Arrays.stream(ZLinkMessageFlowResult.values())
                                .map(ZLinkMessageFlowResult::traceName)
                                .toList()));
        assertEquals(
                Set.of(
                        "node",
                        "channel",
                        "spot",
                        "instance_spot",
                        "actor",
                        "stream",
                        "actor_relocation",
                        "classic_fanout"),
                Set.copyOf(
                        Arrays.stream(ZLinkDispatchErrorSurface.values())
                                .map(ZLinkDispatchErrorSurface::traceName)
                                .toList()));
        assertEquals(
                Set.of("send", "request", "response", "error", "control"),
                Set.copyOf(
                        Arrays.stream(ZLinkDispatchMessageKind.values())
                                .map(ZLinkDispatchMessageKind::traceName)
                                .toList()));
        assertEquals(
                Set.of("reply_error", "fail_caller", "drop"),
                Set.copyOf(
                        Arrays.stream(ZLinkDispatchErrorAction.values())
                                .map(ZLinkDispatchErrorAction::traceName)
                                .toList()));
        assertEquals(
                Set.of(
                        "no_handler",
                        "decode_error",
                        "handler_exception",
                        "invalid_frame",
                        "reply_path_missing",
                        "unexpected_reply",
                        "backpressure",
                        "stale_target",
                        "shutdown",
                        "target_closed",
                        "location_unavailable",
                        "activation_rejected",
                        "activation_timeout"),
                Set.copyOf(
                        Arrays.stream(ZLinkDispatchErrorReason.values())
                                .map(ZLinkDispatchErrorReason::traceName)
                                .toList()));
    }

    @Test
    void flowlessSamplingDoesNotCreateFlowAndFailuresBypassSampling() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.NORMAL);
        options.traceSampleRate(0.0d);
        ZLinkMessageFlowTracer tracer = tracer(options);

        assertNull(systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.current());
        tracer.trace(flow(ZLinkMessageFlowOutcome.RECEIVED));
        tracer.trace(flow(ZLinkMessageFlowOutcome.BACKPRESSURED));

        assertEquals(1L, tracer.tracedCount());
        assertNull(systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.current());
    }

    @Test
    void requestTerminalClassifiesEveryTerminalAndCompletionOwnerEmitsOnce() {
        assertEquals(
                ZLinkMessageFlowResult.SUCCEEDED,
                ZLinkMessageFlowTracer.requestTerminalResult(null, false));
        assertEquals(
                ZLinkMessageFlowResult.FAILED,
                ZLinkMessageFlowTracer.requestTerminalResult(
                        new TimeoutException("deadline"), false));
        assertEquals(
                ZLinkMessageFlowResult.CANCELLED,
                ZLinkMessageFlowTracer.requestTerminalResult(
                        new CancellationException("cancelled"), true));
        assertEquals(
                ZLinkMessageFlowResult.SHUTDOWN,
                ZLinkMessageFlowTracer.requestTerminalResult(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.SHUTTING_DOWN, "shutdown"),
                        false));
        assertEquals(
                ZLinkMessageFlowResult.FAILED,
                ZLinkMessageFlowTracer.requestTerminalResult(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE, "unavailable"),
                        false));

        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        CompletableFuture<String> request = new CompletableFuture<>();
        request.whenComplete(
                (ignored, error) -> {
                    ZLinkMessageFlowTracer.TerminalTracePoint terminal =
                            tracer.beginRequestTerminal(error, request);
                    if (terminal != null) {
                        terminal.trace(flow(ZLinkMessageFlowOutcome.REPLY_RECEIVED));
                    }
                });

        assertTrue(request.complete("ok"));
        assertFalse(request.completeExceptionally(new IllegalStateException("late")));
        assertFalse(request.cancel(false));
        assertEquals(1L, tracer.tracedCount());
    }

    @Test
    void channelRouteKindAcceptsBothChannelSurfacesAndRejectsOtherSurfaces() {
        for (ZLinkDispatchErrorSurface surface : ZLinkDispatchErrorSurface.values()) {
            java.util.function.Supplier<ZLinkMessageFlowEvent> event =
                    () ->
                            new ZLinkMessageFlowEvent(
                                    ZLinkTraceEventId.MESSAGE_FLOW,
                                    ZLinkMessageFlowOutcome.COMPLETED,
                                    ZLinkMessageFlowResult.SUCCEEDED,
                                    surface,
                                    ZLinkDispatchMessageKind.ACTOR_SEND,
                                    "PlaceOrder",
                                    "orders",
                                    ZLinkChannelRouteKind.ROUTE_MESH,
                                    "mesh-a",
                                    "topic-a",
                                    "corr-1",
                                    "source-1",
                                    "target-1",
                                    "server-1",
                                    "spot-1",
                                    "CartSpot",
                                    ZLinkActivationState.READY,
                                    "actor-1",
                                    42L,
                                    0.125d,
                                    null,
                                    null,
                                    null,
                                    null,
                                    null,
                                    null,
                                    null,
                                    null,
                                    17L,
                                    null);
            if (surface == ZLinkDispatchErrorSurface.CHANNEL
                    || surface == ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL) {
                assertEquals(ZLinkChannelRouteKind.ROUTE_MESH, event.get().channelRouteKind());
            } else {
                assertThrows(IllegalArgumentException.class, event::get);
            }
        }
    }

    @Test
    void formatterUsesNormativeEventIdsValuesPrefixAndExactStructuredKeys() {
        ZLinkMessageFlowEvent event =
                new ZLinkMessageFlowEvent(
                        ZLinkTraceEventId.MESSAGE_FLOW,
                        ZLinkMessageFlowOutcome.COMPLETED,
                        ZLinkMessageFlowResult.SUCCEEDED,
                        ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
                        ZLinkDispatchMessageKind.ACTOR_SEND,
                        "PlaceOrder",
                        "orders",
                        ZLinkChannelRouteKind.ROUTE_MESH,
                        "mesh-a",
                        "topic-a",
                        "corr-1",
                        "source-1",
                        "target-1",
                        "server-1",
                        "spot-1",
                        "CartSpot",
                        ZLinkActivationState.READY,
                        "actor-1",
                        42L,
                        0.125d,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        17L,
                        null);

        String line = ZLinkTraceFormat.flowLine(event, 42L);
        assertTrue(line.startsWith("zlink flow: event_id=zlink.message_flow"));
        assertTrue(line.contains(" phase=completed"));
        assertTrue(line.contains(" surface=channel"));
        assertTrue(line.contains(" kind=send"));
        assertTrue(line.contains(" outcome=succeeded"));
        Set<String> expected =
                Set.of(
                        "event_id",
                        "phase",
                        "surface",
                        "kind",
                        "mesh",
                        "channel",
                        "channel_route",
                        "source_rid",
                        "target_rid",
                        "server_rid",
                        "packet",
                        "topic",
                        "spot",
                        "instance_type",
                        "activation_state",
                        "actor",
                        "corr",
                        "outcome",
                        "size");
        assertEquals(
                expected,
                Set.copyOf(
                        Arrays.stream(line.substring("zlink flow: ".length()).split(" "))
                                .map(field -> field.substring(0, field.indexOf('=')))
                                .toList()));
        assertFalse(line.contains("duration_seconds="));

        ZLinkMessageFlowEvent dispatchError =
                ZLinkMessageFlowEvent.dispatchError(
                        ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
                        ZLinkDispatchMessageKind.SEND,
                        "Notice",
                        "events",
                        "topic-a",
                        null,
                        null,
                        null,
                        null,
                        ZLinkDispatchErrorReason.HANDLER_MISSING,
                        ZLinkDispatchErrorAction.DROP,
                        IllegalStateException.class.getSimpleName(),
                        "handler failed");
        String errorLine = ZLinkTraceFormat.flowLine(dispatchError, null);
        assertTrue(errorLine.contains("event_id=zlink.dispatch_error"));
        assertTrue(errorLine.contains("surface=classic_fanout"));
        assertTrue(errorLine.contains("reason=no_handler"));
        assertTrue(errorLine.contains("action=drop"));
        assertTrue(errorLine.contains("error_type=IllegalStateException"));
        assertTrue(errorLine.contains("error_message=handler failed"));
        assertTrue(errorLine.contains("outcome=failed"));
        assertFalse(errorLine.contains(" phase="));
        assertEquals(ZLinkDispatchErrorAction.DROP, dispatchError.errorAction());
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        new ZLinkMessageFlowEvent(
                                ZLinkTraceEventId.DISPATCH_ERROR,
                                null,
                                ZLinkMessageFlowResult.FAILED,
                                ZLinkDispatchErrorSurface.CHANNEL,
                                ZLinkDispatchMessageKind.REQUEST,
                                null,
                                null,
                                ZLinkChannelRouteKind.CLIENT_SERVER,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null,
                                null));
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        new ZLinkMessageFlowEvent(
                                ZLinkMessageFlowOutcome.RECEIVED,
                                ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
                                ZLinkDispatchMessageKind.PUBLISH,
                                "Notice",
                                "events",
                                "topic-a",
                                null,
                                null,
                                null,
                                null,
                                null));
    }

    @Test
    void dispatchErrorTraceAndLogShareSanitizedHandlerDetails() {
        String message =
                "Authorization: Bearer auth Bearer standalone password=p token=t "
                        + "x".repeat(513)
                        + "\n at Secret.Handler";
        String expected =
                ("Authorization: <redacted> Bearer <redacted> password=<redacted> token=<redacted> "
                                + "x".repeat(513))
                        .substring(0, 512);
        ZLinkDispatchErrorReporter.ErrorDetails details =
                ZLinkDispatchErrorReporter.errorDetails(new IllegalStateException(message));
        ZLinkMessageFlowEvent dispatchError =
                ZLinkMessageFlowEvent.dispatchError(
                        ZLinkDispatchErrorSurface.CHANNEL,
                        ZLinkDispatchMessageKind.SEND,
                        "Notice",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                        ZLinkDispatchErrorAction.DROP,
                        details.type(),
                        details.message());

        String line = ZLinkTraceFormat.flowLine(dispatchError, null);

        assertEquals("IllegalStateException", dispatchError.errorType());
        assertEquals(expected, dispatchError.errorMessage());
        assertTrue(line.contains("error_type=IllegalStateException"));
        assertTrue(line.contains("error_message=" + expected));
        assertFalse(line.contains("Bearer auth"));
        assertFalse(line.contains("Bearer standalone"));
        assertFalse(line.contains("password=p"));
        assertFalse(line.contains("token=t"));
        assertFalse(line.contains("Secret.Handler"));
    }

    @Test
    void reporterKeepsConfigurationExceptionTypeIndependentOfItsMessage() {
        List<String> records = new ArrayList<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        synchronized (records) {
                            records.add(record.getMessage());
                            records.notifyAll();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        boolean useParentHandlers = logger.getUseParentHandlers();
        Level previousLevel = logger.getLevel();
        logger.addHandler(handler);
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        try {
            ZLinkDispatchErrorReporter reporter =
                    new ZLinkDispatchErrorReporter(
                            options(ZLinkMessageFlowLogMode.NORMAL),
                            ZLinkHandlerActivator.reflection(),
                            Runnable::run);
            for (String message :
                    List.of("failed to invoke user-selected message", "configuration failed")) {
                ZLinkConfigurationException error =
                        new ZLinkConfigurationException(
                                message, new IllegalStateException("underlying cause"));
                reporter.report(
                        ZLinkDispatchErrorSurface.CHANNEL,
                        ZLinkDispatchMessageKind.SEND,
                        ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                        ZLinkDispatchErrorAction.DROP,
                        "Notice",
                        "orders",
                        null,
                        null,
                        null,
                        null,
                        null,
                        error);
            }
            awaitRecords(records, 2);
            assertEquals(2, records.size());
            for (String record : records) {
                assertTrue(
                        record.contains(
                                "error_type=" + ZLinkConfigurationException.class.getSimpleName()),
                        record);
            }
        } finally {
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(handler);
            logger.setUseParentHandlers(useParentHandlers);
            logger.setLevel(previousLevel);
        }
    }

    @Test
    void dispatchErrorTraceAndLogKeepEmptyMessage() {
        ZLinkDispatchErrorReporter.ErrorDetails details =
                ZLinkDispatchErrorReporter.errorDetails(new IllegalStateException());
        ZLinkMessageFlowEvent dispatchError =
                ZLinkMessageFlowEvent.dispatchError(
                        ZLinkDispatchErrorSurface.CHANNEL,
                        ZLinkDispatchMessageKind.SEND,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                        ZLinkDispatchErrorAction.DROP,
                        details.type(),
                        details.message());

        String line = ZLinkTraceFormat.flowLine(dispatchError, null);

        assertEquals("IllegalStateException", dispatchError.errorType());
        assertEquals("", dispatchError.errorMessage());
        assertTrue(line.contains("error_type=IllegalStateException"));
        assertTrue(line.contains("error_message="));
    }

    @Test
    void samplingHashMatchesUtf8BytesWithoutAllocatingAnEncodingBuffer() {
        for (String value :
                new String[] {"flow-1", "한국어-흐름", "emoji-🚀", "broken-\ud800-tail", "low-\udc00"}) {
            int expected = 0x811c9dc5;
            for (byte current : value.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
                expected = (expected ^ current & 0xff) * 0x01000193;
            }
            assertEquals(expected, ZLinkMessageFlowTracer.fnv1a(value));
        }
    }

    @Test
    void dispatchErrorsUseContractLogLevels() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.ERRORS));

        assertEquals(
                org.slf4j.event.Level.ERROR,
                tracer.logLevel(
                        error(
                                ZLinkDispatchMessageKind.SEND,
                                ZLinkDispatchErrorReason.HANDLER_EXCEPTION)));
        assertEquals(
                org.slf4j.event.Level.ERROR,
                tracer.logLevel(
                        error(
                                ZLinkDispatchMessageKind.PUBLISH,
                                ZLinkDispatchErrorReason.HANDLER_EXCEPTION)));
        assertEquals(
                org.slf4j.event.Level.WARN,
                tracer.logLevel(
                        error(
                                ZLinkDispatchMessageKind.SEND,
                                ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED)));
        assertEquals(
                org.slf4j.event.Level.WARN,
                tracer.logLevel(
                        error(
                                ZLinkDispatchMessageKind.ACTOR_SEND,
                                ZLinkDispatchErrorReason.INVALID_FRAME)));
        assertEquals(
                org.slf4j.event.Level.DEBUG,
                tracer.logLevel(
                        error(
                                ZLinkDispatchMessageKind.PUBLISH,
                                ZLinkDispatchErrorReason.HANDLER_MISSING)));
        assertEquals(
                org.slf4j.event.Level.ERROR,
                tracer.logLevel(
                        error(
                                ZLinkDispatchMessageKind.REQUEST,
                                ZLinkDispatchErrorReason.HANDLER_MISSING)));
    }

    private static ZLinkMessageFlowEvent error(
            ZLinkDispatchMessageKind kind, ZLinkDispatchErrorReason reason) {
        return ZLinkMessageFlowEvent.dispatchError(
                ZLinkDispatchErrorSurface.CHANNEL,
                kind,
                "PlaceOrder",
                "orders",
                null,
                "corr-1",
                null,
                null,
                null,
                reason,
                ZLinkDispatchErrorAction.DROP,
                null,
                null);
    }
}
