package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.ServerSocket;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkActorSpotJoinRelocationPolicyTest {
    private static final AtomicInteger admissions = new AtomicInteger();
    private static volatile CompletableFuture<ZLinkActorJoinCompletion> completion;

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void disabledPolicyRejectsOnlyCrossNodeJoin(boolean mixedFactories) throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var wireRequests = new AtomicInteger();
        var targetBackend =
                ZLinkFrameworkRuntimeTestAccess.withMeshNodeDecorator(
                        new ZLinkJavaBackendAdapterFactory(),
                        node -> countCanonicalJoinRequests(node, wireRequests));
        try (var target =
                ZLinkFrameworkRuntimeTestAccess.startHost(
                        options("target", locations, relocations, mixedFactories), targetBackend)) {
            ready(target);
            await(target.spotManager().getOrCreate("room", "room").submit());
            create(target, "local", "player", "target");
            admissions.set(0);

            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, join(target, "local"));
            assertEquals(1, admissions.get());
            assertEquals(0, wireRequests.get());

            try (var source =
                    ZLinkFrameworkRuntimeTestAccess.start(
                            options("source", locations, relocations, mixedFactories))) {
                ready(source);
                create(source, "remote", "player", "source");
                admissions.set(0);

                var failed =
                        assertInstanceOf(
                                ZLinkActorJoinCompletion.Failed.class, join(source, "remote"));
                assertEquals(ZLinkFrameworkErrorKind.REJECTED, failed.kind());
                assertEquals(0, admissions.get());
                assertEquals(0, wireRequests.get());

                if (mixedFactories) {
                    create(source, "enabled", "enabled-player", "source");
                    assertInstanceOf(
                            ZLinkActorJoinCompletion.Rejected.class, join(source, "enabled"));
                    assertEquals(1, admissions.get());
                    assertEquals(1, wireRequests.get());
                }
            }
        }
    }

    private static ZLinkInternalMeshNode countCanonicalJoinRequests(
            ZLinkInternalMeshNode node, AtomicInteger requests) {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("setCanonicalActorJoinHandler")) {
                                var handler =
                                        (ZLinkInternalMeshNode.CanonicalActorJoinHandler)
                                                arguments[0];
                                node.setCanonicalActorJoinHandler(
                                        (sourceRid, request) -> {
                                            requests.incrementAndGet();
                                            return handler.admit(sourceRid, request);
                                        });
                                return null;
                            }
                            try {
                                return method.invoke(node, arguments);
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    private static void create(
            ZLinkFrameworkRuntime host, String actor, String type, String expectedRid)
            throws Exception {
        var created =
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class,
                        await(host.actorManager().create(actor, type).submit()));
        assertEquals(RoutingId.from(expectedRid), created.actor().nodeRid());
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            boolean mixedFactories)
            throws Exception {
        final int port;
        try (var socket = new ServerSocket(0)) {
            port = socket.getLocalPort();
        }
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var mesh =
                options.addRouteMesh("game")
                        .setRoutingId(RoutingId.from(rid))
                        .listen("tcp://127.0.0.1:" + port)
                        .setPlacementWeight(rid.equals("source") ? 100 : 1);
        mesh.channelName("game").server();
        var objects = mesh.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        if (mixedFactories) {
            objects.addActorFactory(
                    "enabled-player",
                    Player.class,
                    Factory.class,
                    factory -> factory.recreateOnRelocation());
        }
        return options;
    }

    private static void ready(ZLinkFrameworkRuntime host) throws Exception {
        await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(host));
        assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(host)));
    }

    private static ZLinkActorJoinCompletion join(ZLinkFrameworkRuntime host, String actor)
            throws Exception {
        completion = new CompletableFuture<>();
        assertEquals(
                "scheduled",
                await(
                        host.actorClient()
                                .requestToActor(actor, new Join("room"))
                                .submit(String.class)));
        return await(completion);
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
    }

    public record Join(String spot) {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            completion.complete(result);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {
            context.handlers().addHandler(EntryJoin.class);
        }
    }

    public static final class EntryJoin
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Entry entry, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(request.spot(), request).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            admissions.incrementAndGet();
            return CompletableFuture.completedFuture(
                    "enabled".equals(actorId)
                            ? ZLinkSpotActorJoinResult.reject()
                            : ZLinkSpotActorJoinResult.accept());
        }
    }
}
