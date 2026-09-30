package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;

/**
 * Spec 05-spot-actor-membership §4: after a same-node {@code JoinSpot}, the target Actor processes
 * pending messages only once the Join completion callback has ended. The target Spot's {@code
 * OnActorJoin} accepts first, which already routes new arrivals to the target Spot; those arrivals
 * must wait behind the deferred-Join barrier that the source Spot's actor queue still holds, not
 * run on a fresh target queue ahead of the completion.
 */
final class ZLinkSameNodeJoinBarrierTest {
    private static final String ACTOR_ID = "player-1";
    private static final String TARGET_SPOT_ID = "target-room";
    private static final CountDownLatch TARGET_JOINED = new CountDownLatch(1);
    private static final CountDownLatch LIFECYCLE_HELD = new CountDownLatch(1);
    private static final CompletableFuture<Void> LIFECYCLE_RELEASE = new CompletableFuture<>();
    private static final AtomicBoolean JOIN_COMPLETED = new AtomicBoolean();

    @Test
    void arrivalDuringSameNodeJoinWaitsForCompletionCallback() throws Exception {
        AtomicInteger meshJoinSubmissions = new AtomicInteger();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        // OnJoinedActor의 완료를 보류하여 Join completion 전 수신을 확인한다.
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("game");
        node.listen("inproc://same-node-join-barrier-" + System.nanoTime())
                .setRoutingId(RoutingId.from("same-node-join-barrier"));
        var objects = node.objects().server();
        objects.addEntrySpot(EntrySpot.class);
        objects.addSpotFactory("target", TargetSpot.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, observeMeshJoins(meshJoinSubmissions))) {
            var targetCreated =
                    runtime.spotManager()
                            .getOrCreate(TARGET_SPOT_ID, "target")
                            .submit()
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);
            ZLinkActorCreateResult.Created created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            runtime.actorManager()
                                    .create(ACTOR_ID, "player")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(3, TimeUnit.SECONDS));

            String scheduled =
                    runtime.actorClient()
                            .requestToActor(
                                    created.actor().actorId(), new JoinRequest(TARGET_SPOT_ID))
                            .timeout(Duration.ofSeconds(3))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);
            assertEquals("scheduled", scheduled);

            // OnJoinedActor가 아직 완료되지 않은 동안 Actor 수신을 제출한다.
            assertTrue(TARGET_JOINED.await(3, TimeUnit.SECONDS));
            assertTrue(LIFECYCLE_HELD.await(3, TimeUnit.SECONDS));
            CompletableFuture<Boolean> close =
                    runtime.spotManager().close(targetCreated.spot()).toCompletableFuture();

            CompletableFuture<String> probe =
                    runtime.actorClient()
                            .requestToActor(created.actor().actorId(), new ProbeRequest())
                            .timeout(Duration.ofSeconds(10))
                            .submit(String.class)
                            .toCompletableFuture();
            //  Unfixed: the arrival runs at once on the target Spot and
            //  replies before the completion callback. Fixed: it cannot run
            //  while the Join is open, so no reply arrives until the hold is
            //  released below.
            String early;
            try {
                early = probe.get(2, TimeUnit.SECONDS);
            } catch (TimeoutException heldBehindJoin) {
                early = null;
            }
            LIFECYCLE_RELEASE.complete(null);
            assertFalse(close.get(5, TimeUnit.SECONDS));
            String reply = early != null ? early : probe.get(5, TimeUnit.SECONDS);
            assertEquals(
                    TARGET_SPOT_ID + ":completed",
                    reply,
                    "an arrival during a same-node Join must dispatch only after "
                            + "the Join completion callback ended");
            assertEquals(
                    0,
                    meshJoinSubmissions.get(),
                    "same-node public Actor Join must use one Framework local path without Mesh Join records");
        }
    }

    static ZLinkBackendAdapterProvider observeMeshJoins(AtomicInteger submissions) {
        var delegate = new ZLinkJavaBackendAdapterFactory();
        Map<ZLinkBackendObject, ZLinkBackendObject> originals =
                java.util.Collections.synchronizedMap(new IdentityHashMap<>());
        return (ZLinkBackendAdapterProvider)
                Proxy.newProxyInstance(
                        ZLinkBackendAdapterProvider.class.getClassLoader(),
                        new Class<?>[] {ZLinkBackendAdapterProvider.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("admissionTimeout")) {
                                return (Function<ZLinkBackendObject, Duration>)
                                        backend ->
                                                delegate.admissionTimeout()
                                                        .apply(
                                                                originals.getOrDefault(
                                                                        backend, backend));
                            }
                            if (method.getName().equals("createMeshAdapter")) {
                                ZLinkMeshBackendAdapter adapter =
                                        (ZLinkMeshBackendAdapter)
                                                invoke(method, delegate, arguments);
                                return (ZLinkMeshBackendAdapter)
                                        (context, name) -> {
                                            ZLinkInternalMeshNode mesh =
                                                    adapter.createMeshNode(context, name);
                                            ZLinkInternalSpotNode spot =
                                                    (ZLinkInternalSpotNode)
                                                            Proxy.newProxyInstance(
                                                                    ZLinkInternalSpotNode.class
                                                                            .getClassLoader(),
                                                                    new Class<?>[] {
                                                                        ZLinkInternalSpotNode.class
                                                                    },
                                                                    (p, m, a) -> {
                                                                        if (m.getName()
                                                                                        .equals(
                                                                                                "joinActor")
                                                                                || m.getName()
                                                                                        .equals(
                                                                                                "joinActorEntrySpot")) {
                                                                            submissions
                                                                                    .incrementAndGet();
                                                                        }
                                                                        return invoke(
                                                                                m,
                                                                                mesh.spotNode(),
                                                                                a);
                                                                    });
                                            ZLinkInternalMeshNode observed =
                                                    (ZLinkInternalMeshNode)
                                                            Proxy.newProxyInstance(
                                                                    ZLinkInternalMeshNode.class
                                                                            .getClassLoader(),
                                                                    new Class<?>[] {
                                                                        ZLinkInternalMeshNode.class
                                                                    },
                                                                    (p, m, a) ->
                                                                            m.getName()
                                                                                            .equals(
                                                                                                    "spotNode")
                                                                                    ? spot
                                                                                    : invoke(
                                                                                            m, mesh,
                                                                                            a));
                                            originals.put(observed, mesh);
                                            return observed;
                                        };
                            }
                            return invoke(method, delegate, arguments);
                        });
    }

    private static Object invoke(java.lang.reflect.Method method, Object target, Object[] arguments)
            throws Throwable {
        try {
            return method.invoke(target, arguments);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, completion);
            JOIN_COMPLETED.set(true);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class EntrySpot implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public EntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
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

    public static final class TargetSpot implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public TargetSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            // target lifecycle 완료 전에는 Join completion과 Actor 수신을 진행하지 않는다.
            TARGET_JOINED.countDown();
            LIFECYCLE_HELD.countDown();
            return LIFECYCLE_RELEASE;
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record JoinRequest(String spotId) {}

    public record ProbeRequest() {}

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<EntrySpot, Player, JoinRequest, String> {
        @Override
        public CompletionStage<String> handle(
                EntrySpot spot, Player actor, ZLinkMessageContext context, JoinRequest request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorRequestHandler<TargetSpot, Player, ProbeRequest, String> {
        @Override
        public CompletionStage<String> handle(
                TargetSpot spot, Player actor, ZLinkMessageContext context, ProbeRequest request) {
            return CompletableFuture.completedFuture(
                    actor.context().spotId().orElse("none")
                            + (JOIN_COMPLETED.get() ? ":completed" : ":incomplete"));
        }
    }
}
