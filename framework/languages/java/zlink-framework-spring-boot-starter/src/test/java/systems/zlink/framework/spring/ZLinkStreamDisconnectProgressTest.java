package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.micrometer.core.instrument.Counter;
import io.micrometer.core.instrument.simple.SimpleMeterRegistry;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.context.annotation.Import;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkStreamDisconnectProgressTest {
    private static final Duration OBSERVATION_TIMEOUT = Duration.ofSeconds(3);
    private static final String STREAM_NODE = "gateway";
    private static final String ACTOR_TYPE = "player";
    private static final String ACTOR_ID = "disconnect-progress-player";
    private static final String BIND_PACKET = "Bind";
    private static final String HOLD_PACKET = "Hold";
    private static final String CLOSED_METRIC = "zlink.stream.connections.closed";
    private static final long METRIC_POLL_MILLIS = 1;
    private static Observations observations;

    @Test
    void physicalDisconnectNotifiesActorWhileSessionHandlerAwaits() throws Exception {
        observations = new Observations();
        boolean actorNotifiedBeforeRelease = false;
        try (var application = new AnnotationConfigApplicationContext()) {
            application.registerBean(SimpleMeterRegistry.class, SimpleMeterRegistry::new);
            application.register(TestConfig.class);
            application.refresh();
            var runtime = application.getBean(ZLinkFrameworkRuntime.class);
            var metrics = application.getBean(SimpleMeterRegistry.class);
            int port =
                    URI.create(
                                    runtime.listenerStatus(ZLinkListenerKind.STREAM, STREAM_NODE)
                                            .endpoint())
                            .getPort();
            try (var client = new Socket("127.0.0.1", port)) {
                client.setSoTimeout((int) OBSERVATION_TIMEOUT.toMillis());
                send(client, BIND_PACKET, 1L, "\"" + ACTOR_ID + "\"");
                // The public bind emits a bound-control frame, then its request reply.
                readFrame(client);
                readFrame(client);
                observations.bound.get(OBSERVATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                Counter opened =
                        metrics.find("zlink.stream.connections.opened")
                                .tag("transport", "tcp")
                                .counter();
                assertTrue(
                        opened != null && opened.count() >= 1,
                        "The public metrics provider must observe the bound connection before Hold");
                send(client, HOLD_PACKET, null, "{}");
                observations.handlerEntered.get(
                        OBSERVATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                client.close();

                awaitPhysicalCloseMetric(metrics);
                assertFalse(observations.releaseHandler.isDone());
                assertFalse(
                        observations.sessionDisconnected.isDone(),
                        "Session user callbacks must stay serialized behind the held handler");
                System.out.println(
                        "STREAM_CAUSAL physical-close-witness=micrometer "
                                + "metric="
                                + CLOSED_METRIC
                                + " held-handler=pending");
                try {
                    observations.actorDisconnected.get(
                            OBSERVATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                    actorNotifiedBeforeRelease = true;
                } catch (TimeoutException expectedRed) {
                    // Release and verify the same physical close produces the callback below.
                } finally {
                    observations.releaseHandler.complete(null);
                }
                observations.actorDisconnected.get(
                        OBSERVATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                observations.sessionDisconnected.get(
                        OBSERVATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                assertEquals(1, observations.actorDisconnectCount.get());
                System.out.println(
                        "STREAM_CAUSAL after-release-actor-callback=observed "
                                + "actor-callback-before-release="
                                + actorNotifiedBeforeRelease);
                assertTrue(
                        actorNotifiedBeforeRelease,
                        "Physical disconnect was observed by the server, but its Actor notification "
                                + "waited for the unrelated Session handler to finish");
            } finally {
                observations.releaseHandler.complete(null);
            }
        }
    }

    private static void awaitPhysicalCloseMetric(SimpleMeterRegistry metrics) throws Exception {
        long deadline = System.nanoTime() + OBSERVATION_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            Counter counter =
                    metrics.find(CLOSED_METRIC)
                            .tag("transport", "tcp")
                            .tag("close_reason", "transport_error")
                            .counter();
            if (counter != null && counter.count() >= 1) return;
            Thread.sleep(METRIC_POLL_MILLIS);
        }
        throw new AssertionError(
                "No server physical-disconnect metric was observed; "
                        + "this run cannot establish a handler/cleanup dependency");
    }

    private static void send(Socket client, String packet, Long sequence, String json)
            throws Exception {
        byte[] header =
                ZLinkStreamHeaderCodec.encode(
                        new ZLinkStreamHeader(
                                sequence == null
                                        ? ZLinkStreamMessageKind.SEND
                                        : ZLinkStreamMessageKind.REQUEST,
                                ZLinkStreamCodec.RAW,
                                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                Optional.ofNullable(sequence),
                                packet,
                                Map.of(),
                                Optional.empty()));
        byte[] body = json.getBytes(StandardCharsets.UTF_8);
        ByteBuffer frame = ByteBuffer.allocate(6 + header.length + body.length);
        frame.putShort((short) header.length).putInt(body.length).put(header).put(body);
        client.getOutputStream().write(frame.array());
        client.getOutputStream().flush();
    }

    private static void readFrame(Socket client) throws Exception {
        byte[] prefix = client.getInputStream().readNBytes(6);
        assertEquals(6, prefix.length);
        ByteBuffer lengths = ByteBuffer.wrap(prefix);
        int headerLength = Short.toUnsignedInt(lengths.getShort());
        int bodyLength = lengths.getInt();
        assertEquals(headerLength, client.getInputStream().readNBytes(headerLength).length);
        assertEquals(bodyLength, client.getInputStream().readNBytes(bodyLength).length);
    }

    private static final class Observations {
        final CompletableFuture<Void> bound = new CompletableFuture<>();
        final CompletableFuture<Void> handlerEntered = new CompletableFuture<>();
        final CompletableFuture<Void> releaseHandler = new CompletableFuture<>();
        final CompletableFuture<Void> actorDisconnected = new CompletableFuture<>();
        final CompletableFuture<Void> sessionDisconnected = new CompletableFuture<>();
        final AtomicInteger actorDisconnectCount = new AtomicInteger();
    }

    @Configuration(proxyBeanMethods = false)
    @EnableZLinkFramework
    @Import(ZLinkMetricsAutoConfiguration.class)
    static class TestConfig {
        @Bean
        ZLinkFrameworkConfigurer topology() {
            return options -> {
                options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                options.addLocationStore(new ZLinkInMemoryLocationStore());
                var node = options.addRouteMesh("game");
                node.listen("tcp://127.0.0.1:0");
                node.objects()
                        .server()
                        .addEntrySpot(DisconnectSpot.class)
                        .addActorFactory(
                                ACTOR_TYPE,
                                Player.class,
                                PlayerFactory.class,
                                factory -> factory.disableRelocation());
                var stream = options.addStreamNode(STREAM_NODE);
                stream.bind("tcp://127.0.0.1:0");
                stream.enableActorDispatch();
                stream.registerSession(HeldSession.class);
            };
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class DisconnectSpot implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public DisconnectSpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnectActor(Player actor) {
            observations.actorDisconnectCount.incrementAndGet();
            observations.actorDisconnected.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class HeldSession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkActorManager actors;

        public HeldSession(ZLinkSessionContext context, ZLinkActorManager actors) {
            this.context = context;
            this.actors = actors;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            observations.sessionDisconnected.complete(null);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            if (HOLD_PACKET.equals(dispatch.packetName())) {
                observations.handlerEntered.complete(null);
                return observations.releaseHandler;
            }
            return actors.getOrCreate(payload.decode(String.class), ACTOR_TYPE)
                    .submit()
                    .thenCompose(
                            result -> {
                                var actor =
                                        switch (result) {
                                            case ZLinkActorCreateResult.Created created ->
                                                    created.actor();
                                            case ZLinkActorCreateResult.Existing existing ->
                                                    existing.actor();
                                            case ZLinkActorCreateResult.Rejected rejected ->
                                                    throw new IllegalStateException(
                                                            "Actor creation rejected: "
                                                                    + rejected.reply());
                                        };
                                return context.actors().bindOrGet(actor);
                            })
                    .thenRun(
                            () -> {
                                context.client().reply("bound").submit();
                                observations.bound.complete(null);
                            });
        }
    }
}
