package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.streams.*;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.stream.connector.*;

import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.stream.Stream;

final class SameNodeDuplicateBindIntegrationTest {
    private static final BlockingQueue<TestSession> sessions = new LinkedBlockingQueue<>();
    private static Player player;

    static Stream<Arguments> cases() throws Exception {
        Path directory = Path.of("").toAbsolutePath();
        while (directory != null) {
            Path fixture =
                    directory.resolve("runtime/conformance/bound-session-relocation-v1.json");
            if (Files.exists(fixture)) {
                var cases =
                        new ObjectMapper()
                                .readTree(Files.readString(fixture))
                                .get("sameNodeDuplicateBind");
                var result = Stream.<Arguments>builder();
                for (var scenario : cases) {
                    result.add(
                            Arguments.of(
                                    scenario.get("rejectionMessage").asText(),
                                    scenario.get("accepted").asBoolean(),
                                    scenario.get("callbackCount").asInt(),
                                    scenario.get("replacementCount").asInt()));
                }
                return result.build();
            }
            directory = directory.getParent();
        }
        throw new IllegalStateException("Shared binding fixture was not found");
    }

    @ParameterizedTest
    @MethodSource("cases")
    void sameNodeDifferentSessionUsesActorDuplicateBindDecision(
            String rejection, boolean accepted, int callbackCount, int replacementCount)
            throws Exception {
        boolean reject = !accepted;
        sessions.clear();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var mesh = options.addRouteMesh("game").listen("tcp://127.0.0.1:0");
        mesh.objects()
                .server()
                .addEntrySpot(Entry.class)
                .addActorFactory("player", Player.class, Factory.class, f -> f.disableRelocation());
        options.addStreamNode("clients")
                .bind("tcp://127.0.0.1:0")
                .enableActorDispatch()
                .registerSession(TestSession.class);
        try (var runtime =
                RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            var created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            await(
                                    runtime.actorManager()
                                            .create("duplicate-player", "player")
                                            .submit()));
            player.rejection = rejection;
            try (var first = connect(runtime)) {
                var current = sessions.poll(3, TimeUnit.SECONDS);
                assertNotNull(current);
                var bound = await(current.context.actors().bind(created.actor()));
                assertSame(bound, await(current.context.actors().bindOrGet(created.actor())));
                assertEquals(0, player.callbacks.get());
                var oldActor = await(first.bound());
                assertEquals(
                        "before",
                        await(oldActor.request(new ActorProbe("before")).submit(String.class)));
                try (var second = connect(runtime)) {
                    var incoming = sessions.poll(3, TimeUnit.SECONDS);
                    assertNotNull(incoming);
                    if (reject) {
                        var failure =
                                assertThrows(
                                        ExecutionException.class,
                                        () ->
                                                await(
                                                        incoming.context
                                                                .actors()
                                                                .bind(created.actor())));
                        assertEquals(
                                ZLinkFrameworkErrorKind.REJECTED,
                                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                        .kind());
                        assertTrue(
                                current.context
                                        .actors()
                                        .find(created.actor().actorId())
                                        .isPresent());
                        assertTrue(
                                incoming.context
                                        .actors()
                                        .find(created.actor().actorId())
                                        .isEmpty());
                        assertFalse(current.replaced.isDone());
                        assertEquals(
                                "current",
                                await(
                                        oldActor.request(new ActorProbe("current"))
                                                .submit(String.class)));
                    } else {
                        await(incoming.context.actors().bind(created.actor()));
                        assertEquals(callbackCount, player.callbacks.get());
                        assertEquals(created.actor().actorId(), await(current.replaced));
                        assertTrue(
                                incoming.context
                                        .actors()
                                        .find(created.actor().actorId())
                                        .isPresent());
                        var successor = await(second.bound());
                        assertEquals(
                                "current",
                                await(
                                        successor
                                                .request(new ActorProbe("current"))
                                                .submit(String.class)));
                        var delivered = new CompletableFuture<String>();
                        var oldPushes = new AtomicInteger();
                        first.connector()
                                .on(
                                        "String",
                                        String.class,
                                        message -> {
                                            oldPushes.incrementAndGet();
                                            return CompletableFuture.completedFuture(null);
                                        });
                        second.connector()
                                .on(
                                        "String",
                                        String.class,
                                        message -> {
                                            delivered.complete(message.payload());
                                            return CompletableFuture.completedFuture(null);
                                        });
                        await(player.context.boundSession().send("current").submit());
                        assertEquals("current", await(delivered));
                        assertEquals(0, oldPushes.get());
                        var retired =
                                CompletableFuture.completedFuture(oldActor)
                                        .thenCompose(
                                                actor ->
                                                        actor.request(new ActorProbe("retired"))
                                                                .submit(String.class));
                        var failure = assertThrows(ExecutionException.class, () -> await(retired));
                        assertInstanceOf(ZLinkStreamException.class, failure.getCause());
                        await(first.closed());
                        assertEquals(2, player.ingress.get());
                    }
                    assertEquals(callbackCount, player.callbacks.get());
                    assertEquals(replacementCount, current.replaced.isDone() ? 1 : 0);
                }
            }
        }
    }

    private record Client(
            ZLinkStreamConnector connector,
            CompletableFuture<ZLinkStreamActor> bound,
            CompletableFuture<Void> closed)
            implements AutoCloseable {
        public void close() throws Exception {
            await(connector.close().submit());
        }
    }

    private static Client connect(ZLinkFrameworkRuntime runtime) throws Exception {
        var endpoint =
                URI.create(runtime.listenerStatus(ZLinkListenerKind.STREAM, "clients").endpoint());
        var options =
                new ZLinkStreamConnectorOptions(
                        endpoint,
                        ZLinkStreamDispatchMode.IMMEDIATE,
                        Duration.ofSeconds(30),
                        3,
                        Duration.ofSeconds(5),
                        65536,
                        false,
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        false,
                        Duration.ofMillis(250),
                        Duration.ofSeconds(5),
                        2.0);
        var connector = ZLinkStreamConnectorFactory.create(options);
        var bound = new CompletableFuture<ZLinkStreamActor>();
        var closed = new CompletableFuture<Void>();
        connector.onActorBound(
                actor -> {
                    bound.complete(actor);
                    return CompletableFuture.completedFuture(null);
                });
        connector.onDisconnected(
                reason -> {
                    closed.complete(null);
                    return CompletableFuture.completedFuture(null);
                });
        await(connector.connect().submit());
        return new Client(connector, bound, closed);
    }

    private static <T> T await(CompletionStage<T> result) throws Exception {
        return result.toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;
        final AtomicInteger callbacks = new AtomicInteger();
        final AtomicInteger ingress = new AtomicInteger();
        String rejection;

        Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }

        public CompletionStage<Optional<String>> onDuplicateBind(
                ZLinkSessionBindingIdentity current,
                ZLinkSessionBindingIdentity incoming,
                ZLinkActorCallbackCancellation cancellation) {
            callbacks.incrementAndGet();
            return CompletableFuture.completedFuture(
                    Optional.ofNullable(rejection).filter(message -> !message.isEmpty()));
        }
    }

    public record ActorProbe(String value) {}

    public static final class ProbeHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<String> handle(Player actor, ActorProbe request) {
            actor.ingress.incrementAndGet();
            return CompletableFuture.completedFuture(request.value());
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            player = new Player(context);
            return CompletableFuture.completedFuture(player);
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TestSession implements ZLinkSession {
        final ZLinkSessionContext context;
        final CompletableFuture<String> replaced = new CompletableFuture<>();

        public TestSession(ZLinkSessionContext context) {
            this.context = context;
        }

        public ZLinkSessionContext context() {
            return context;
        }

        public CompletionStage<Void> onConnected() {
            sessions.add(this);
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage message) {
            return dispatch.actor() == null
                    ? context.client().reply("pong").submit()
                    : dispatch.actor().relay(dispatch, message);
        }

        public CompletionStage<Void> onActorBindingReplaced(String actorId) {
            replaced.complete(actorId);
            return CompletableFuture.completedFuture(null);
        }
    }
}
