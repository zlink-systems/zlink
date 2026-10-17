package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

/** Source-side public admission regression; the follow receiver below is a test owner. */
final class ZLinkLocalActorCutoverAdmissionTest {
    private static final String ACTOR = "local-cutover-player";
    private static final long WAIT_SECONDS = 5;
    private static final AtomicInteger SOURCE_HANDLERS = new AtomicInteger();

    @Test
    void publicLocalSendSucceedsWhenThePostLookupCutIsAcceptedByTheForward() throws Exception {
        SOURCE_HANDLERS.set(0);
        try (var host = ZLinkFrameworkRuntimeTestAccess.start(options())) {
            host.actorManager()
                    .create(ACTOR, "player")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            var actors = (ZLinkActorRuntime) host.actorManager();
            actors.awaitDrainBarrier().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
            var actor = actors.localActor(ACTOR).orElseThrow();
            long generation = actor.context().objectGeneration();
            var queue =
                    actors.actorRelocationLaneAsync(ACTOR)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
            var seal =
                    queue.trySealRelocation()
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS)
                            .orElseThrow();
            var retained =
                    actors.retainActorRelocationCommitAsync(ACTOR, seal)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS)
                            .orElseThrow();
            try {
                assertEquals(List.of(), retained.records(), "the single cut must start quiescent");

                var node = (ZLinkInternalSpotNode) field(field(host, "spots"), "primaryNode");
                var entry = node.entrySpot();
                var original =
                        (ZLinkInternalAsyncSpotDispatchHandler) field(entry, "dispatchHandler");
                var originalFollow =
                        (ZLinkInternalSpotNode.MessageFollowRelayHandler)
                                field(node, "messageFollowRelayHandler");
                assertNotNull(originalFollow);
                var lookups = new AtomicInteger();
                var nativeDispatches = new AtomicInteger();
                var forwards = new AtomicInteger();
                var dispatchTerminal = new CompletableFuture<Void>();
                var accepted = new CompletableFuture<ZLinkServiceFrozenRecordCodec.DecodedActor>();
                byte[] expectedPayload =
                        new ZLinkJsonMessageSerializer().serialize(new Probe(7)).bytes();

                try {
                    // Observe the existing pre-native follow lookup. The forward is not installed
                    // yet.
                    node.setMessageFollowRelayHandler(
                            new ZLinkInternalSpotNode.MessageFollowRelayHandler() {
                                @Override
                                public boolean hasSource(String actorId) {
                                    assertEquals(ACTOR, actorId);
                                    lookups.incrementAndGet();
                                    boolean found = originalFollow.hasSource(actorId);
                                    assertFalse(
                                            found,
                                            "this arrival must first choose native local dispatch");
                                    return found;
                                }

                                @Override
                                public boolean handle(
                                        RoutingId rid,
                                        long sourceGeneration,
                                        ZLinkServiceM6BWireCodec.ActorMessage header,
                                        byte[] journal,
                                        List<Message> parts,
                                        String contentType,
                                        Consumer<List<Message>> reply,
                                        Consumer<Throwable> failure,
                                        Runnable release) {
                                    return originalFollow.handle(
                                            rid,
                                            sourceGeneration,
                                            header,
                                            journal,
                                            parts,
                                            contentType,
                                            reply,
                                            failure,
                                            release);
                                }
                            });

                    var forward =
                            new ZLinkInternalSpotNode.MessageFollowRelayHandler() {
                                @Override
                                public boolean handle(
                                        RoutingId rid,
                                        long sourceGeneration,
                                        ZLinkServiceM6BWireCodec.ActorMessage header,
                                        byte[] journal,
                                        List<Message> parts,
                                        String contentType,
                                        Consumer<List<Message>> reply,
                                        Consumer<Throwable> failure,
                                        Runnable release) {
                                    try {
                                        assertEquals(1, forwards.incrementAndGet());
                                        var record =
                                                ZLinkServiceFrozenRecordCodec.decodeActor(journal);
                                        assertEquals(
                                                header.operationHigh(), record.operationHigh());
                                        assertEquals(header.operationLow(), record.operationLow());
                                        assertTrue(
                                                record.operationHigh() != 0
                                                        || record.operationLow() != 0);
                                        assertEquals(ACTOR, record.actorId());
                                        assertEquals(generation, record.objectGeneration());
                                        assertEquals(
                                                ServiceWireConstants
                                                        .FRAMEWORK_MULTIPART_PACKET_NAME,
                                                record.packetName());
                                        assertEquals(
                                                ServiceWireConstants
                                                        .FRAMEWORK_MULTIPART_CONTENT_TYPE,
                                                record.contentType());
                                        var restored =
                                                ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(
                                                        new ZLinkServiceM6AWireCodec
                                                                .ApplicationPayload(
                                                                record.packetName(),
                                                                record.contentType(),
                                                                record.payload()));
                                        try {
                                            assertEquals(2, restored.size());
                                            assertEquals(2, parts.size());
                                            assertEquals(
                                                    "Probe",
                                                    ZLinkStreamHeaderCodec.decodeOrPlain(
                                                                    restored.getFirst()
                                                                            .toByteArray())
                                                            .packetName());
                                            assertArrayEquals(
                                                    parts.getFirst().toByteArray(),
                                                    restored.getFirst().toByteArray());
                                            assertArrayEquals(
                                                    expectedPayload, restored.get(1).toByteArray());
                                            assertArrayEquals(
                                                    parts.get(1).toByteArray(),
                                                    restored.get(1).toByteArray());
                                        } finally {
                                            restored.forEach(Message::close);
                                        }
                                        accepted.complete(record);
                                        return true;
                                    } finally {
                                        // This receiver owns the forwarded copies, not a target
                                        // application turn.
                                        parts.forEach(Message::close);
                                        release.run();
                                    }
                                }
                            };

                    var wrapper =
                            (ZLinkInternalAsyncSpotDispatchHandler)
                                    Proxy.newProxyInstance(
                                            ZLinkInternalAsyncSpotDispatchHandler.class
                                                    .getClassLoader(),
                                            new Class<?>[] {
                                                ZLinkInternalAsyncSpotDispatchHandler.class
                                            },
                                            (proxy, method, arguments) -> {
                                                if (!method.getName().equals("handleActor")) {
                                                    return invoke(original, method, arguments);
                                                }
                                                @SuppressWarnings("unchecked")
                                                var messages =
                                                        (List<ZLinkBackendActorReceived>)
                                                                arguments[0];
                                                @SuppressWarnings("unchecked")
                                                var candidateAdmission =
                                                        (CompletableFuture<Void>) arguments[1];
                                                return ZLinkHandlerStages.fromStageSupplier(
                                                                () -> {
                                                                    assertEquals(
                                                                            1,
                                                                            lookups.get(),
                                                                            "the RawSpot native choice must already exist");
                                                                    assertEquals(
                                                                            1,
                                                                            nativeDispatches
                                                                                    .incrementAndGet());
                                                                    node
                                                                            .setMessageFollowRelayHandler(
                                                                                    forward);
                                                                    return retained.cut()
                                                                            .thenCompose(
                                                                                    cut -> {
                                                                                        assertEquals(
                                                                                                List
                                                                                                        .of(),
                                                                                                cut
                                                                                                        .records());
                                                                                        return retained
                                                                                                .tryEstablishAndFinishCapture(
                                                                                                        cut);
                                                                                    })
                                                                            .thenCompose(
                                                                                    finished -> {
                                                                                        assertTrue(
                                                                                                finished,
                                                                                                "the quiescent retained cut must finish in one attempt");
                                                                                        return original
                                                                                                .handleActor(
                                                                                                        messages,
                                                                                                        candidateAdmission);
                                                                                    });
                                                                })
                                                        .whenComplete(
                                                                (ignored, failure) -> {
                                                                    if (failure != null)
                                                                        messages.forEach(
                                                                                ZLinkBackendActorReceived
                                                                                        ::close);
                                                                    if (failure != null)
                                                                        dispatchTerminal
                                                                                .completeExceptionally(
                                                                                        failure);
                                                                    else
                                                                        dispatchTerminal.complete(
                                                                                null);
                                                                });
                                            });
                    entry.onDispatchEvent(wrapper);
                    var admission =
                            host.actorClient()
                                    .sendToActor(ACTOR, new Probe(7))
                                    .submit()
                                    .toCompletableFuture();
                    dispatchTerminal.get(WAIT_SECONDS, TimeUnit.SECONDS);
                    accepted.get(WAIT_SECONDS, TimeUnit.SECONDS);
                    assertEquals(1, lookups.get());
                    assertEquals(1, nativeDispatches.get());
                    assertEquals(1, forwards.get());
                    assertEquals(0, SOURCE_HANDLERS.get());
                    assertDoesNotThrow(
                            () -> admission.get(WAIT_SECONDS, TimeUnit.SECONDS),
                            "the original operation was accepted by the forward after its local FIFO candidate rejected it");
                } finally {
                    entry.onDispatchEvent(original);
                    node.setMessageFollowRelayHandler(originalFollow);
                }
            } finally {
                assertTrue(
                        retained.abort().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS),
                        "the test-only cut must restore its source queue before host close");
            }
        }
    }

    private static Object invoke(Object target, java.lang.reflect.Method method, Object[] arguments)
            throws Throwable {
        try {
            return method.invoke(target, arguments);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }

    private static Object field(Object target, String name) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    private static DefaultZLinkFrameworkOptions options() {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new InMemoryRelocationStore());
        var node = options.addRouteMesh("local-cutover");
        node.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from("local-cutover-node"));
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.preserveStateWith(State.class));
        return options;
    }

    public record Probe(int value) {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
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

    public static final class State implements ZLinkActorRelocationAdapter<Player> {
        @Override
        public CompletionStage<byte[]> capture(
                Player actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Lobby implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Lobby(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeHandler
            implements ZLinkEntrySpotActorSendHandler<Lobby, Player, Probe> {
        @Override
        public CompletionStage<Void> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Probe probe) {
            SOURCE_HANDLERS.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }
}
