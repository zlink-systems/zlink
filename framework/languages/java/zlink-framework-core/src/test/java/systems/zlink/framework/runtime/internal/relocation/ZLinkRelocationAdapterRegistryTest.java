package systems.zlink.framework.runtime.internal.relocation;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

final class ZLinkRelocationAdapterRegistryTest {
    @Test
    void mapsSnapshotAdapterByStableActorType() {
        ZLinkFrameworkRegistration registration =
            new ZLinkFrameworkRegistration();
        MeshNodeRegistration node = new MeshNodeRegistration("game");
        node.listen("inproc://game");
        node.objects().server().addActorFactory(
            "player",
            TestActor.class,
            TestActorFactory.class,
            factory -> factory.preserveStateWith(TestActorAdapter.class));
        node.validate();
        registration.meshNodes().add(node);

        ZLinkRelocationAdapterRegistry adapters =
            new ZLinkRelocationAdapterRegistry(
                registration,
                ZLinkHandlerActivator.reflection());

        assertTrue(adapters.hasActorAdapter("player"));
        assertArrayEquals(
            new byte[] {4, 2},
            adapters.captureActor(
                    "player",
                    new TestActor(),
                    () -> false)
                .toCompletableFuture()
                .join());
    }

    public static final class TestActor implements ZLinkActor {
        @Override
        public ZLinkActorContext context() {
            return null;
        }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new TestActor());
        }
    }

    public static final class TestActorAdapter
        implements ZLinkActorRelocationAdapter<TestActor> {
        @Override
        public CompletionStage<byte[]> capture(
            TestActor actor,
            ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[] {4, 2});
        }

        @Override
        public CompletionStage<Void> restore(
            TestActor actor,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
