package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.testfixtures.subscriptionmissing.MissingTopicSubscription;

import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

final class ZLinkSpotContextActivationTest {
    @Test
    void missingRuntimeContextFailsActivationAfterHostStartup() {
        assertInvalidActivation(InvalidContextSpot.class);
    }

    @Test
    void configureSubscriptionFailureIsInvalidOperationAtThePublicManager() {
        assertInvalidActivation(InvalidSubscriptionSpot.class);
    }

    private static <T extends ZLinkSpot<?>> void assertInvalidActivation(Class<T> spotType) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("context-mesh");
        node.listen("inproc://context-activation-" + UUID.randomUUID());
        node.channelName("context-mesh").server();
        node.objects()
                .server()
                .addSpotFactory(
                        "invalid-context", spotType, factory -> factory.disableRelocation());
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    runtime.spotManager()
                                            .getOrCreate("context-spot", "invalid-context")
                                            .submit()
                                            .toCompletableFuture()
                                            .join());
            var error = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
            assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, error.kind());
            assertFalse(
                    runtime.spotManager()
                            .find("context-spot")
                            .toCompletableFuture()
                            .join()
                            .isPresent());
        }
    }

    public static final class InvalidSubscriptionSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public InvalidSubscriptionSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(MissingTopicSubscription.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class InvalidContextSpot implements ZLinkSpot<ZLinkActor> {
        public InvalidContextSpot(ZLinkSpotContext context) {}

        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
