package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;

final class ZLinkUserSpotActivationFailurePreservationTest {
    private static final IllegalStateException CREATE_FAILURE = new IllegalStateException();
    private static final TimeoutException INITIALIZE_FAILURE = new TimeoutException();

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void creationPreservesTheOriginalMessageLessCallbackFailure(boolean create) {
        Class<? extends ZLinkSpot<?>> type =
                create ? CreateFailureSpot.class : InitializeFailureSpot.class;
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var mesh = options.addRouteMesh("failure-mesh");
        mesh.listen("inproc://activation-failure-" + UUID.randomUUID());
        mesh.channelName("failure-mesh").server();
        mesh.objects()
                .server()
                .addSpotFactory("failure", type, factory -> factory.disableRelocation());
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            var failed =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    runtime.spotManager()
                                            .getOrCreate("failure-spot", "failure")
                                            .request("activation-request")
                                            .submit()
                                            .toCompletableFuture()
                                            .join());
            Throwable original = failed;
            while (original.getCause() != null) original = original.getCause();
            assertSame(
                    type == CreateFailureSpot.class ? CREATE_FAILURE : INITIALIZE_FAILURE,
                    original);
            assertNull(original.getMessage());
            assertFalse(
                    runtime.spotManager()
                            .find("failure-spot")
                            .toCompletableFuture()
                            .join()
                            .isPresent());
        }
    }

    public abstract static class FailedSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        protected FailedSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
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

    public static final class CreateFailureSpot extends FailedSpot {
        public CreateFailureSpot(ZLinkSpotContext context) {
            super(context);
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            throw CREATE_FAILURE;
        }
    }

    public static final class InitializeFailureSpot extends FailedSpot {
        public InitializeFailureSpot(ZLinkSpotContext context) {
            super(context);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return CompletableFuture.failedFuture(INITIALIZE_FAILURE);
        }
    }
}
