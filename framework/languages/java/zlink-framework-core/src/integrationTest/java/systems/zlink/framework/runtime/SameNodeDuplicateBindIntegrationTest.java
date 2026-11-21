package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.streams.*;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.stream.connector.*;

import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
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
            player.runtime = ZLinkFrameworkRuntimeTestAccess.actors(runtime);
            try (var first = connect(runtime)) {
                var current = sessions.poll(3, TimeUnit.SECONDS);
                assertNotNull(current);
                var bound = await(current.context.actors().bind(created.actor()));
                assertSame(bound, await(current.context.actors().bindOrGet(created.actor())));
                await(player.boundEntered);
                assertEquals(1, player.boundCallbacks.get());
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
                        var binding = incoming.context.actors().bind(created.actor());
                        await(binding);
                        assertEquals(
                                List.of(created.actor().actorId()), await(player.queriedActorIds));
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
                    assertEquals(accepted ? 2 : 1, player.boundCallbacks.get());
                }
            }
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void duplicateBindCallbackCanQueryRuntimeActors(boolean remote) throws Exception {
        sessions.clear();
        var locations = new ZLinkInMemoryLocationStore();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addRouteMesh("game")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addEntrySpot(Entry.class)
                .addActorFactory("player", Player.class, Factory.class, f -> f.disableRelocation());
        options.addStreamNode("clients")
                .bind("tcp://127.0.0.1:0")
                .enableActorDispatch()
                .registerSession(TestSession.class);
        try (var owner =
                RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            var created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            await(owner.actorManager().create("query-player", "player").submit()));
            player.runtime = ZLinkFrameworkRuntimeTestAccess.actors(owner);
            ZLinkFrameworkRuntime incomingOwner = owner;
            if (remote) {
                var gatewayOptions = new DefaultZLinkFrameworkOptions();
                gatewayOptions.addLocationStore(locations);
                gatewayOptions.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                var gateway = gatewayOptions.addRouteMesh("game").listen("tcp://127.0.0.1:0");
                gateway.objects().client();
                gateway.peerConnections()
                        .connect(
                                owner.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game")
                                        .endpoint());
                gatewayOptions
                        .addStreamNode("clients")
                        .bind("tcp://127.0.0.1:0")
                        .enableActorDispatch()
                        .registerSession(TestSession.class);
                incomingOwner =
                        RuntimeTestSupport.startFramework(
                                gatewayOptions, new ZLinkJavaBackendAdapterFactory());
            }
            try {
                try (var first = connect(owner)) {
                    var current = sessions.poll(3, TimeUnit.SECONDS);
                    assertNotNull(current);
                    await(current.context.actors().bind(created.actor()));
                    try (var second = connect(incomingOwner)) {
                        var incoming = sessions.poll(3, TimeUnit.SECONDS);
                        assertNotNull(incoming);
                        var binding = incoming.context.actors().bind(created.actor());
                        assertAll(
                                () -> await(binding),
                                () ->
                                        assertEquals(
                                                List.of(created.actor().actorId()),
                                                await(player.queriedActorIds)));
                        assertEquals(1, player.callbacks.get());
                        assertEquals(created.actor().actorId(), await(current.replaced));
                        assertTrue(
                                incoming.context
                                        .actors()
                                        .find(created.actor().actorId())
                                        .isPresent());
                    }
                }
            } finally {
                if (remote) incomingOwner.close();
            }
        }
    }

    @org.junit.jupiter.api.Test
    void sessionBoundOldIncarnationIsInvalidOperation() throws Exception {
        sessionBoundNotificationKeepsSuccessfulBind("recreated");
    }

    @ParameterizedTest
    @ValueSource(
            strings = {
                "success",
                "throw",
                "failed",
                "error",
                "pending",
                "timeout",
                "shutdown",
                "actor-destroy",
                "destroyed"
            })
    void sessionBoundNotificationKeepsSuccessfulBind(String outcome) throws Exception {
        sessions.clear();
        var options = new DefaultZLinkFrameworkOptions();
        if (outcome.equals("timeout")) {
            options.setSessionReplacementCallbackTimeout(Duration.ofMillis(100));
        }
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addRouteMesh("game")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addEntrySpot(Entry.class)
                .addActorFactory("player", Player.class, Factory.class, f -> f.disableRelocation());
        options.addStreamNode("clients")
                .bind("tcp://127.0.0.1:0")
                .enableActorDispatch()
                .registerSession(TestSession.class);
        try (var failures = new RuntimeFailures();
                var runtime =
                        RuntimeTestSupport.startFramework(
                                options, new ZLinkJavaBackendAdapterFactory())) {
            var created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            await(
                                    runtime.actorManager()
                                            .create("bound-player", "player")
                                            .submit()));
            player.boundOutcome = outcome;
            player.runtime = ZLinkFrameworkRuntimeTestAccess.actors(runtime);
            try (var client = connect(runtime)) {
                var session = sessions.poll(3, TimeUnit.SECONDS);
                assertNotNull(session);
                if (outcome.equals("destroyed") || outcome.equals("recreated")) {
                    assertTrue(await(runtime.actorManager().destroy(created.actor())));
                    if (outcome.equals("recreated")) {
                        await(runtime.actorManager().create("bound-player", "player").submit());
                    }
                    var failure =
                            assertThrows(
                                    ExecutionException.class,
                                    () -> await(session.context.actors().bind(created.actor())));
                    assertEquals(
                            outcome.equals("recreated")
                                    ? ZLinkFrameworkErrorKind.INVALID_OPERATION
                                    : ZLinkFrameworkErrorKind.UNAVAILABLE,
                            assertInstanceOf(
                                            ZLinkFrameworkException.class,
                                            failure.getCause(),
                                            failure.getCause().toString())
                                    .kind());
                    assertEquals(0, player.boundCallbacks.get());
                    return;
                }
                await(session.context.actors().bind(created.actor()));
                CompletionStage<Boolean> destruction = null;
                try {
                    var identity = await(player.boundEntered);
                    assertEquals(
                            identity,
                            player.runtime
                                    .boundSessionRoute(player)
                                    .orElseThrow()
                                    .sessionBindingIdentity());
                    assertTrue(
                            session.context.actors().find(created.actor().actorId()).isPresent());
                    assertEquals(1, player.boundCallbacks.get());
                    var cancellation = await(player.boundCancellation);
                    if (outcome.equals("timeout")) {
                        assertInstanceOf(
                                TimeoutException.class, failures.next.poll(3, TimeUnit.SECONDS));
                        assertTrue(cancellation.isCancellationRequested());
                    } else if (outcome.equals("actor-destroy")) {
                        destruction = runtime.actorManager().destroy(created.actor());
                        assertTimeoutPreemptively(
                                Duration.ofSeconds(3),
                                () -> {
                                    while (!cancellation.isCancellationRequested()) Thread.sleep(1);
                                });
                    } else if (outcome.equals("shutdown")) {
                        player.runtime.beginDrain();
                        assertTrue(cancellation.isCancellationRequested());
                    } else if (outcome.equals("error")) {
                        assertInstanceOf(
                                AssertionError.class, failures.next.poll(3, TimeUnit.SECONDS));
                        assertFalse(cancellation.isCancellationRequested());
                    } else if (outcome.equals("throw") || outcome.equals("failed")) {
                        assertInstanceOf(
                                IllegalStateException.class,
                                failures.next.poll(3, TimeUnit.SECONDS));
                        assertFalse(cancellation.isCancellationRequested());
                    }
                } finally {
                    if (outcome.equals("timeout")
                            || outcome.equals("shutdown")
                            || outcome.equals("actor-destroy"))
                        player.boundTerminal.completeExceptionally(
                                new CancellationException("bound callback cancelled"));
                    else player.boundTerminal.complete(null);
                }
                if (destruction != null) assertTrue(await(destruction));
                var actor = await(client.bound());
                if (!outcome.equals("shutdown") && !outcome.equals("actor-destroy")) {
                    assertEquals(
                            "after-notification",
                            await(
                                    actor.request(new ActorProbe("after-notification"))
                                            .submit(String.class)));
                }
                assertEquals(1, player.boundCallbacks.get());
                assertEquals(
                        outcome.equals("throw")
                                        || outcome.equals("failed")
                                        || outcome.equals("error")
                                        || outcome.equals("timeout")
                                ? 1
                                : 0,
                        failures.observed.size());
            }
        }
    }

    private static final class RuntimeFailures extends java.util.logging.Handler
            implements AutoCloseable {
        final java.util.logging.Logger logger =
                java.util.logging.Logger.getLogger("systems.zlink.framework.runtime");
        final BlockingQueue<Throwable> next = new LinkedBlockingQueue<>();
        final List<Throwable> observed = new CopyOnWriteArrayList<>();

        RuntimeFailures() {
            logger.addHandler(this);
        }

        public void publish(java.util.logging.LogRecord record) {
            if (record.getMessage().equals("ZLink runtime task failed")) {
                observed.add(record.getThrown());
                next.add(record.getThrown());
            }
        }

        public void flush() {}

        public void close() {
            logger.removeHandler(this);
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
        ZLinkActorRuntime runtime;
        final CompletableFuture<List<String>> queriedActorIds = new CompletableFuture<>();

        Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }

        final AtomicInteger boundCallbacks = new AtomicInteger();
        final CompletableFuture<ZLinkSessionBindingIdentity> boundEntered =
                new CompletableFuture<>();
        final CompletableFuture<Void> boundTerminal = new CompletableFuture<>();
        final CompletableFuture<ZLinkActorCallbackCancellation> boundCancellation =
                new CompletableFuture<>();
        String boundOutcome = "success";

        public CompletionStage<Void> onSessionBound(
                ZLinkSessionBindingIdentity incoming, ZLinkActorCallbackCancellation cancellation) {
            boundCallbacks.incrementAndGet();
            assertTrue(runtime.activeActorIds().contains(context.actorId()));
            boundCancellation.complete(cancellation);
            boundEntered.complete(incoming);
            return switch (boundOutcome) {
                case "throw" -> throw new IllegalStateException("bound callback failed");
                case "error" -> throw new AssertionError("bound callback error");
                case "failed" ->
                        CompletableFuture.failedFuture(
                                new IllegalStateException("bound stage failed"));
                case "pending", "timeout", "shutdown", "actor-destroy" -> boundTerminal;
                default -> CompletableFuture.completedFuture(null);
            };
        }

        public CompletionStage<Optional<String>> onDuplicateBind(
                ZLinkSessionBindingIdentity current,
                ZLinkSessionBindingIdentity incoming,
                ZLinkActorCallbackCancellation cancellation) {
            callbacks.incrementAndGet();
            var query =
                    CompletableFuture.completedFuture(context.actorId())
                            .thenApply(actorId -> runtime.activeActorIds());
            query.whenComplete(
                    (ids, failure) -> {
                        if (failure == null) queriedActorIds.complete(ids);
                        else queriedActorIds.completeExceptionally(failure);
                    });
            return query.thenApply(
                    ids -> Optional.ofNullable(rejection).filter(message -> !message.isEmpty()));
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
