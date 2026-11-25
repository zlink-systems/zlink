package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.stream.connector.ZLinkStreamCloseReason;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamErrorCode;
import systems.zlink.stream.connector.ZLinkStreamException;

import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkStreamSessionCreationFailureTest {
    private static final Duration DEADLINE = Duration.ofSeconds(2);
    private static final String NODE = "creation-failure-gateway";
    private static final String FAILURE = "session constructor failed for this peer";
    private static final String PACKET = "CreationFailureProbe";
    private static final String LATER_PACKET = "LaterCreationFailureProbe";

    @Test
    void connectionReadyFailureClosesOnlyThatPeerAndLogsOnce() throws Exception {
        try (var fixture = new Fixture(false)) {
            await(fixture.healthy.connect().submit());
            fixture.probe.awaitConnected(1);
            assertEquals(
                    "healthy",
                    await(fixture.healthy.request(PACKET, "healthy").submit(String.class)));
            assertAll(
                    () ->
                            assertEquals(
                                    ZLinkStreamCloseReason.TRANSPORT_ERROR,
                                    fixture.connectFailedPeer()),
                    fixture::assertCreationDiagnostic,
                    fixture::assertHealthyPeers,
                    () -> assertFalse(fixture.failedFlow.isDone()),
                    () -> assertEquals(3, fixture.probe.attempts.get()));
        }
    }

    @Test
    void firstRequestFailureRepliesBeforeCloseAndTracesFailure() throws Exception {
        try (var fixture = new Fixture(true)) {
            await(fixture.healthy.connect().submit());
            fixture.probe.awaitConnected(1);
            assertEquals(
                    "healthy",
                    await(fixture.healthy.request(PACKET, "healthy").submit(String.class)));
            await(fixture.held.connect().submit());
            await(fixture.probe.monitorEntered);
            await(fixture.failed.connect().submit());
            var request = fixture.failed.request(PACKET, "failed").submit(String.class);
            assertAll(
                    () -> {
                        var failure = assertThrows(ExecutionException.class, () -> await(request));
                        var remote =
                                assertInstanceOf(ZLinkStreamException.class, failure.getCause());
                        assertEquals(ZLinkStreamErrorCode.REMOTE_ERROR, remote.errorCode());
                        assertTrue(remote.getCause().getMessage().startsWith("internal_failure:"));
                    },
                    () ->
                            assertEquals(
                                    ZLinkStreamCloseReason.TRANSPORT_ERROR,
                                    await(fixture.disconnected)),
                    fixture::assertCreationDiagnostic,
                    () -> {
                        String flow = await(fixture.failedFlow);
                        assertAll(
                                () -> assertTrue(flow.contains("outcome=failed"), flow),
                                () ->
                                        assertTrue(
                                                flow.contains("event_id=zlink.dispatch_error"),
                                                flow),
                                () -> assertTrue(flow.contains("packet=" + PACKET), flow),
                                () -> assertTrue(flow.contains("corr=1"), flow),
                                () ->
                                        assertTrue(
                                                flow.contains(
                                                        "error_type=java.lang.IllegalStateException"),
                                                flow),
                                () -> assertTrue(flow.contains(FAILURE), flow));
                    },
                    fixture::assertHealthyPeers);
            fixture.probe.releaseMonitor.complete(null);
            fixture.probe.awaitConnected(2);
            assertEquals("held", await(fixture.held.request(PACKET, "held").submit(String.class)));
            assertEquals(4, fixture.probe.attempts.get());
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(DEADLINE.toMillis(), TimeUnit.MILLISECONDS);
    }

    @EnableZLinkFramework
    static class Enabled {}

    public static final class Probe {
        final boolean holdMonitor;
        final AtomicInteger attempts = new AtomicInteger();
        final CompletableFuture<Void> monitorEntered = new CompletableFuture<>();
        final CompletableFuture<Void> releaseMonitor = new CompletableFuture<>();
        final LinkedBlockingQueue<Integer> connected = new LinkedBlockingQueue<>();

        Probe(boolean holdMonitor) {
            this.holdMonitor = holdMonitor;
        }

        void awaitConnected(int attempt) throws InterruptedException {
            assertEquals(attempt, connected.poll(DEADLINE.toMillis(), TimeUnit.MILLISECONDS));
        }
    }

    public static final class Session implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final Probe probe;
        private final int attempt;

        public Session(ZLinkSessionContext context, Probe probe) {
            this.probe = probe;
            attempt = probe.attempts.incrementAndGet();
            if (probe.holdMonitor && attempt == 2) {
                probe.monitorEntered.complete(null);
                probe.releaseMonitor.join();
            }
            if (attempt == (probe.holdMonitor ? 3 : 2)) {
                throw new IllegalStateException(
                        FAILURE, new IllegalArgumentException("underlying constructor dependency"));
            }
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            probe.connected.add(attempt);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            return context.client().reply(payload.decode(String.class)).submit();
        }
    }

    private static final class Fixture implements AutoCloseable {
        final Probe probe;
        final AnnotationConfigApplicationContext app = new AnnotationConfigApplicationContext();
        final CompletableFuture<ZLinkStreamCloseReason> disconnected = new CompletableFuture<>();
        final CompletableFuture<String> failedFlow = new CompletableFuture<>();
        final CompletableFuture<Void> laterFlow = new CompletableFuture<>();
        final List<LogRecord> errors = new CopyOnWriteArrayList<>();
        final Logger runtimeLogger =
                Logger.getLogger("systems.zlink.framework.runtime.streams.ZLinkStreamRuntime");
        final Logger flowLogger =
                Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        final Handler logCapture =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if (record.getLevel().intValue() >= Level.SEVERE.intValue()
                                && record.getMessage().contains(NODE)) {
                            errors.add(record);
                        }
                        if (record.getMessage().contains("reason=session_creation_failed")) {
                            failedFlow.complete(record.getMessage());
                        }
                        if (record.getMessage().contains("phase=replied")
                                && record.getMessage().contains("packet=" + LATER_PACKET)) {
                            laterFlow.complete(null);
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        final ZLinkStreamConnector healthy;
        final ZLinkStreamConnector held;
        final ZLinkStreamConnector failed;
        final ZLinkStreamConnector later;

        Fixture(boolean holdMonitor) {
            probe = new Probe(holdMonitor);
            app.registerBean(Probe.class, () -> probe);
            app.registerBean(
                    ZLinkFrameworkConfigurer.class,
                    () ->
                            options -> {
                                options.configureDispatch()
                                        .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                                options.addStreamNode(NODE)
                                        .bind("ws://127.0.0.1:0")
                                        .registerSession(Session.class);
                            });
            app.register(Enabled.class);
            app.refresh();
            var host = app.getBean(ZLinkFrameworkRuntime.class);
            var endpoint =
                    URI.create(host.listenerStatus(ZLinkListenerKind.STREAM, NODE).endpoint());
            healthy = newClient(endpoint);
            held = newClient(endpoint);
            failed = newClient(endpoint);
            later = newClient(endpoint);
            failed.onDisconnected(
                    event -> {
                        disconnected.complete(event.closeReason());
                        return CompletableFuture.completedFuture(null);
                    });
            runtimeLogger.addHandler(logCapture);
            flowLogger.addHandler(logCapture);
        }

        ZLinkStreamCloseReason connectFailedPeer() throws Exception {
            await(failed.connect().submit());
            return await(disconnected);
        }

        void assertCreationDiagnostic() {
            assertEquals(1, errors.size(), errors.toString());
            LogRecord record = errors.getFirst();
            assertTrue(record.getMessage().contains(Session.class.getName()), record.getMessage());
            assertTrue(record.getMessage().contains(FAILURE), record.getMessage());
            Throwable cause = record.getThrown();
            while (cause != null && !FAILURE.equals(cause.getMessage())) {
                cause = cause.getCause();
            }
            assertEquals(
                    FAILURE, assertInstanceOf(IllegalStateException.class, cause).getMessage());
        }

        void assertHealthyPeers() throws Exception {
            assertEquals(
                    "still healthy",
                    await(healthy.request(PACKET, "still healthy").submit(String.class)));
            await(later.connect().submit());
            if (!probe.holdMonitor) probe.awaitConnected(3);
            assertEquals("later", await(later.request(LATER_PACKET, "later").submit(String.class)));
            await(laterFlow);
            if (probe.holdMonitor) probe.awaitConnected(4);
        }

        @Override
        public void close() throws Exception {
            probe.releaseMonitor.complete(null);
            try {
                for (var client : List.of(later, failed, held, healthy)) {
                    await(client.close().submit());
                }
            } finally {
                runtimeLogger.removeHandler(logCapture);
                flowLogger.removeHandler(logCapture);
                app.close();
            }
        }

        private static ZLinkStreamConnector newClient(URI endpoint) {
            return ZLinkStreamConnectorFactory.create(
                    new ZLinkStreamConnectorOptions(
                            endpoint,
                            ZLinkStreamDispatchMode.IMMEDIATE,
                            DEADLINE,
                            1,
                            DEADLINE,
                            64 * 1024,
                            false,
                            DEADLINE,
                            DEADLINE,
                            false,
                            Duration.ofMillis(250),
                            DEADLINE,
                            2.0));
        }
    }
}
