package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

final class ZLinkActorTransferRegistryTest {
    @Test
    void registeredAdapterCapturesAndRestoresDomainState() {
        ZLinkActorTransferRegistry registry = new ZLinkActorTransferRegistry(
            java.util.Map.of("stateful", StatefulAdapter.class),
            ZLinkHandlerActivator.reflection());
        TestActor source = new TestActor(contextFor("actor-1"), "version-7");

        ZLinkActorTransferRegistry.TransferState state =
            registry.transferOut("stateful", source).toCompletableFuture().join();
        TestActor target = (TestActor) registry.transferIn(
            "stateful",
            "actor-1",
            contextFor("actor-1"),
            state.state(),
            TestActorFactory.class).toCompletableFuture().join();

        assertEquals("stateful", state.adapterKey());
        assertEquals("version-7", target.state);
    }

    @Test
    void missingAdapterUsesEmptyStateAndFactorySignal() {
        ZLinkActorTransferRegistry registry = new ZLinkActorTransferRegistry(
            java.util.Map.of(),
            ZLinkHandlerActivator.reflection());

        ZLinkActorTransferRegistry.TransferState state =
            registry.transferOut(
                "stateless",
                new TestActor(contextFor("actor-1"), "ignored"))
                .toCompletableFuture().join();

        assertNull(state.adapterKey());
        assertEquals(true, state.state().isEmpty());
        assertNull(registry.transferIn(
            "stateless",
            "actor-1",
            contextFor("actor-1"),
            state.state(),
            TestActorFactory.class).toCompletableFuture().join());
    }

    public static final class StatefulAdapter
        implements ZLinkActorRelocationAdapter<TestActor> {
        @Override
        public java.util.concurrent.CompletionStage<byte[]> capture(
            TestActor actor,
            ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(
                actor.state.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public java.util.concurrent.CompletionStage<Void> restore(
            TestActor actor,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            actor.state = new String(state, StandardCharsets.UTF_8);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public java.util.concurrent.CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                new TestActor(context, "loaded-elsewhere"));
        }
    }

    static final class TestActor implements ZLinkActor {
        private final ZLinkActorContext context;
        private String state;

        TestActor(ZLinkActorContext context, String state) {
            this.context = context;
            this.state = state;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    private static ZLinkActorContext contextFor(String actorId) {
        return new ZLinkActorContext() {
            @Override public String actorId() { return actorId; }
            @Override public long objectGeneration() { return 1L; }
            @Override public String meshName() { return "test"; }
            @Override public java.util.Optional<String> spotId() {
                return java.util.Optional.empty();
            }
            @Override public systems.zlink.framework.actors.ZLinkBoundSession boundSession() {
                return null;
            }
            @Override public ZLinkActorJoinCall joinSpot(String spotId) {
                throw new UnsupportedOperationException();
            }
            @Override public ZLinkActorJoinCall joinSpot(String spotId, Object request) {
                throw new UnsupportedOperationException();
            }
            @Override public ZLinkActorJoinCall joinEntrySpot() {
                throw new UnsupportedOperationException();
            }
            @Override public ZLinkActorJoinCall joinEntrySpot(Object request) {
                throw new UnsupportedOperationException();
            }
        };
    }
}
