package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkHandlerOwnerStartupTest {
    @Test
    void packageScanChecksOwnerBeforeAnySpotIsActivated() {
        assertOwner(BaseSpot.class, DerivedSpot.class, OwnerHandler.class);
        assertOwner(ActorOwnerSpot.class, DerivedActorSpot.class, ActorOwnerHandler.class);
    }

    private static void assertOwner(
            Class<? extends ZLinkSpot<?>> owner,
            Class<? extends ZLinkSpot<?>> derived,
            Class<?> handler) {
        var mismatch = options(derived);
        var error = assertThrows(ZLinkConfigurationException.class, mismatch::validate);
        for (var type : new Class<?>[] {handler, owner, derived}) {
            assertTrue(error.getMessage().contains(type.getName()), error.getMessage());
        }
        assertDoesNotThrow(options(owner)::validate);
        assertDoesNotThrow(options(owner, derived)::validate);
        var separate = options(owner);
        separate.addRouteMesh("other-owner")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addSpotFactory("other", derived, factory -> factory.disableRelocation());
        assertDoesNotThrow(separate::validate);
    }

    @SafeVarargs
    private static DefaultZLinkFrameworkOptions options(Class<? extends ZLinkSpot<?>>... types) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addHandlersFromPackageOf(ZLinkHandlerOwnerStartupTest.class);
        var server =
                options.addRouteMesh("handler-owner")
                        .listen("tcp://127.0.0.1:0")
                        .objects()
                        .server();
        for (int index = 0; index < types.length; index++)
            server.addSpotFactory(
                    "owner" + index, types[index], factory -> factory.disableRelocation());
        return options;
    }

    private abstract static class EmptySpot implements ZLinkSpot<OwnerActor> {
        public ZLinkSpotContext context() {
            return null;
        }

        public CompletionStage<Void> onJoinedActor(OwnerActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(OwnerActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static class BaseSpot extends EmptySpot {}

    private static class ActorOwnerSpot extends EmptySpot {}

    private static final class DerivedActorSpot extends ActorOwnerSpot {}

    private static final class OwnerActor implements ZLinkActor {
        public ZLinkActorContext context() {
            return null;
        }
    }

    private static final class ActorOwnerHandler
            implements ZLinkSpotActorSendHandler<ActorOwnerSpot, OwnerActor, String> {
        public CompletionStage<Void> handle(
                ActorOwnerSpot spot,
                OwnerActor actor,
                ZLinkMessageContext context,
                String message) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class DerivedSpot extends BaseSpot {}

    private static final class OwnerHandler implements ZLinkSpotPacketHandler<BaseSpot, String> {
        public CompletionStage<Void> handle(BaseSpot spot, String message) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
