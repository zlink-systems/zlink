package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class ZLinkStreamSessionShutdownTest {
    private static final Duration DEADLINE = Duration.ofSeconds(2);

    @Test
    void deadlineStopsHostWhileConnectedCallbackIsPending() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.pauseConnectorReceive();
            var lateSend = fixture.session.client().send(new ProbePacket("late"));
            var shutdown = fixture.host.shutdown(DEADLINE).toCompletableFuture();
            ZLinkFrameworkTerminationResult result;
            try {
                result = shutdown.get(DEADLINE.toMillis() + 500, TimeUnit.MILLISECONDS);
            } catch (TimeoutException failure) {
                throw new AssertionError("Shutdown still pending after the host deadline", failure);
            }
            assertEquals(
                    new ZLinkFrameworkTerminationResult(
                            ZLinkFrameworkTerminationOutcome.FORCE_STOPPED,
                            ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED),
                    result);
            assertEquals(List.of(), fixture.probe.callbacks);
            fixture.resumeConnector.complete(null);
            await(fixture.disconnected);

            fixture.probe.release.complete(null);
            await(fixture.probe.finished);
            var send = assertThrows(ExecutionException.class, () -> await(lateSend.submit()));
            assertEquals(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(ZLinkFrameworkException.class, send.getCause()).kind());
            await(fixture.session.close());
            assertEquals(List.of("connected"), fixture.probe.callbacks);
        }
    }

    @Test
    void hostDrainsWhenConnectedCallbackFinishesBeforeDeadline() throws Exception {
        try (var fixture = new Fixture()) {
            var shutdown = fixture.host.shutdown(DEADLINE);
            fixture.probe.release.complete(null);
            assertEquals(
                    new ZLinkFrameworkTerminationResult(
                            ZLinkFrameworkTerminationOutcome.STOPPED,
                            ZLinkFrameworkTerminationReason.NONE),
                    await(shutdown));
            await(fixture.disconnected);
            var lifecycle =
                    fixture.probe.callbacks.stream()
                            .filter(callback -> !callback.equals("dispatch"))
                            .toList();
            assertEquals(List.of("connected", "disconnected"), lifecycle);
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
    }

    public record ProbePacket(String value) {}

    public static final class Probe {
        final CompletableFuture<ZLinkSessionContext> entered = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();
        final CompletableFuture<Void> finished = new CompletableFuture<>();
        final List<String> callbacks = new CopyOnWriteArrayList<>();
    }

    public static final class Session implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final Probe probe;

        public Session(ZLinkSessionContext context, Probe probe) {
            this.context = context;
            this.probe = probe;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            probe.entered.complete(context);
            return probe.release.thenRun(
                    () -> {
                        probe.callbacks.add("connected");
                        probe.finished.complete(null);
                    });
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            probe.callbacks.add("disconnected");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            probe.callbacks.add("error");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            probe.callbacks.add("dispatch");
            return CompletableFuture.completedFuture(null);
        }
    }

    @EnableZLinkFramework
    static class Enabled {}

    private static final class Fixture implements AutoCloseable {
        final AnnotationConfigApplicationContext app = new AnnotationConfigApplicationContext();
        final Probe probe = new Probe();
        final CompletableFuture<Void> disconnected = new CompletableFuture<>();
        final ZLinkFrameworkRuntime host;
        final ZLinkSessionContext session;
        final CompletableFuture<Void> resumeConnector = new CompletableFuture<>();
        private ZLinkStreamConnector client;

        Fixture() throws Exception {
            try {
                app.registerBean(Probe.class, () -> probe);
                app.registerBean(
                        ZLinkFrameworkConfigurer.class,
                        () ->
                                options ->
                                        options.addStreamNode("probe")
                                                .bind("ws://127.0.0.1:0")
                                                .registerSession(Session.class));
                app.register(Enabled.class);
                app.refresh();
                host = app.getBean(ZLinkFrameworkRuntime.class);
                await(host.setMessageFlowModeAsync(ZLinkMessageFlowLogMode.NORMAL));
                var endpoint =
                        URI.create(
                                host.listenerStatus(ZLinkListenerKind.STREAM, "probe").endpoint());
                client =
                        ZLinkStreamConnectorFactory.create(
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
                client.onDisconnected(
                        event -> {
                            disconnected.complete(null);
                            return CompletableFuture.completedFuture(null);
                        });
                await(client.connect().submit());
                await(client.send(new ProbePacket("prime")).submit());
                session = await(probe.entered);
            } catch (Exception | AssertionError failure) {
                try {
                    close();
                } catch (Exception cleanup) {
                    failure.addSuppressed(cleanup);
                }
                throw failure;
            }
        }

        void pauseConnectorReceive() throws Exception {
            var connectorEntered = new CompletableFuture<Void>();
            client.on(
                    ProbePacket.class,
                    message -> {
                        connectorEntered.complete(null);
                        resumeConnector.join();
                        return CompletableFuture.completedFuture(null);
                    });
            await(session.client().send(new ProbePacket("pause")).submit());
            await(connectorEntered);
        }

        @Override
        public void close() throws Exception {
            resumeConnector.complete(null);
            probe.release.complete(null);
            try {
                if (client != null) {
                    await(client.close().submit());
                }
            } finally {
                app.close();
            }
        }
    }
}
