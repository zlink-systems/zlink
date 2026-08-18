package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorBaseDeltaRelocationAdapter;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

/**
 * Item 3/4 of the base/delta transfer pipeline (spec 15 §5): the target
 * restores a base snapshot then applies the delta, and a failed delta
 * discards the partially restored instance and retries the whole
 * restoreBase-&gt;applyDelta sequence exactly once on a fresh instance before
 * failing explicitly.
 */
final class ZLinkActorRuntimeBaseDeltaRelocationTest {
    @Test
    void restoresBaseThenAppliesDeltaOnTheFirstAttempt() {
        AtomicInteger restoreBaseCalls = new AtomicInteger();
        AtomicInteger applyDeltaCalls = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        ZLinkRelocationAdapterRegistry adapters = adapters(
            (actor, base, cancellation) -> {
                restoreBaseCalls.incrementAndGet();
                actor.state().append("base:").append(new String(base));
                return CompletableFuture.completedFuture(null);
            },
            (actor, delta, cancellation) -> {
                applyDeltaCalls.incrementAndGet();
                actor.state().append("|delta:").append(new String(delta));
                return CompletableFuture.completedFuture(null);
            });

        var prepared = runtime.prepareRelocatedActor(
                "actor-base-delta",
                "player",
                "the-delta".getBytes(),
                false,
                adapters,
                () -> false,
                null,
                "the-base".getBytes())
            .toCompletableFuture().join();

        assertEquals(1, restoreBaseCalls.get());
        assertEquals(1, applyDeltaCalls.get());
        assertEquals("base:the-base|delta:the-delta",
            ((TestActor) prepared.actor()).state().toString());
    }

    @Test
    void applyDeltaFailureDiscardsTheInstanceAndRetriesOnceOnAFreshInstance() {
        AtomicInteger restoreBaseCalls = new AtomicInteger();
        AtomicInteger applyDeltaCalls = new AtomicInteger();
        java.util.List<TestActor> restoredInstances =
            new java.util.concurrent.CopyOnWriteArrayList<>();
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        ZLinkRelocationAdapterRegistry adapters = adapters(
            (actor, base, cancellation) -> {
                restoreBaseCalls.incrementAndGet();
                restoredInstances.add(actor);
                return CompletableFuture.completedFuture(null);
            },
            (actor, delta, cancellation) -> {
                if (applyDeltaCalls.incrementAndGet() == 1) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException("first delta application fails"));
                }
                return CompletableFuture.completedFuture(null);
            });

        var prepared = runtime.prepareRelocatedActor(
                "actor-base-delta-retry",
                "player",
                "the-delta".getBytes(),
                false,
                adapters,
                () -> false,
                null,
                "the-base".getBytes())
            .toCompletableFuture().join();

        assertEquals(2, restoreBaseCalls.get(),
            "restoreBase runs again on the retry");
        assertEquals(2, applyDeltaCalls.get());
        assertEquals(2, restoredInstances.size());
        assertNotSame(restoredInstances.get(0), restoredInstances.get(1),
            "no partial reuse: the retry restores a fresh instance");
        assertNotSame(restoredInstances.get(1), null);
        assertEquals(prepared.actor(), restoredInstances.get(1));
    }

    @Test
    void applyDeltaFailureTwiceIsAnExplicitFailure() {
        AtomicInteger applyDeltaCalls = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys);
        ZLinkRelocationAdapterRegistry adapters = adapters(
            (actor, base, cancellation) -> CompletableFuture.completedFuture(null),
            (actor, delta, cancellation) -> {
                applyDeltaCalls.incrementAndGet();
                return CompletableFuture.failedFuture(
                    new IllegalStateException("delta application always fails"));
            });

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> runtime.prepareRelocatedActor(
                    "actor-base-delta-fail",
                    "player",
                    "the-delta".getBytes(),
                    false,
                    adapters,
                    () -> false,
                    null,
                    "the-base".getBytes())
                .toCompletableFuture().join());

        assertEquals(2, applyDeltaCalls.get(),
            "exactly one retry before an explicit failure");
        assertTrue(failure.getCause().getMessage()
            .contains("delta application always fails"));
        assertEquals(1, destroys.get(),
            "the backend actor reference is released after the final failure");
    }

    private interface RestoreBase {
        CompletionStage<Void> restore(
            TestActor actor, byte[] base, ZLinkRelocationCancellation cancellation);
    }

    private interface ApplyDelta {
        CompletionStage<Void> apply(
            TestActor actor, byte[] delta, ZLinkRelocationCancellation cancellation);
    }

    private static ZLinkRelocationAdapterRegistry adapters(
        RestoreBase restoreBase,
        ApplyDelta applyDelta) {
        TestActorAdapter.RESTORE_BASE.set(restoreBase);
        TestActorAdapter.APPLY_DELTA.set(applyDelta);
        ZLinkFrameworkRegistration registration = new ZLinkFrameworkRegistration();
        MeshNodeRegistration node = new MeshNodeRegistration("game");
        node.listen("inproc://game-base-delta");
        node.objects().server().addActorFactory(
            "player",
            TestActor.class,
            TestActorFactory.class,
            factory -> factory.preserveStateWith(TestActorAdapter.class));
        node.validate();
        registration.meshNodes().add(node);
        return new ZLinkRelocationAdapterRegistry(
            registration, ZLinkHandlerActivator.reflection());
    }

    private static ZLinkActorRuntime runtime(AtomicInteger destroys) {
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> RoutingId.from("node-a");
                case "createActor" -> {
                    ((Message) arguments[1]).close();
                    yield new ZLinkBackendActorRef(
                        RoutingId.from("node-a"),
                        (String) arguments[0],
                        7);
                }
                case "destroyActor" -> {
                    destroys.incrementAndGet();
                    yield CompletableFuture.completedFuture(null);
                }
                case "close" -> null;
                default -> defaultValue(method.getReturnType());
            });
        return new ZLinkActorRuntime(
            node,
            Map.of("player", TestActorFactory.class),
            Duration.ofSeconds(5),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection());
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) return null;
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0F;
        if (type == double.class) return 0D;
        if (type == char.class) return '\0';
        return null;
    }

    public static final class TestActor implements ZLinkActor {
        private final StringBuilder state = new StringBuilder();

        @Override
        public ZLinkActorContext context() {
            return null;
        }

        StringBuilder state() {
            return state;
        }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new TestActor());
        }
    }

    public static final class TestActorAdapter
        implements ZLinkActorBaseDeltaRelocationAdapter<TestActor> {
        static final java.util.concurrent.atomic.AtomicReference<RestoreBase>
            RESTORE_BASE = new java.util.concurrent.atomic.AtomicReference<>();
        static final java.util.concurrent.atomic.AtomicReference<ApplyDelta>
            APPLY_DELTA = new java.util.concurrent.atomic.AtomicReference<>();

        @Override
        public CompletionStage<byte[]> capture(
            TestActor actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
            TestActor actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<byte[]> captureBase(
            TestActor actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<byte[]> captureDelta(
            TestActor actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restoreBase(
            TestActor actor, byte[] base, ZLinkRelocationCancellation cancellation) {
            return RESTORE_BASE.get().restore(actor, base, cancellation);
        }

        @Override
        public CompletionStage<Void> applyDelta(
            TestActor actor, byte[] delta, ZLinkRelocationCancellation cancellation) {
            return APPLY_DELTA.get().apply(actor, delta, cancellation);
        }
    }
}
