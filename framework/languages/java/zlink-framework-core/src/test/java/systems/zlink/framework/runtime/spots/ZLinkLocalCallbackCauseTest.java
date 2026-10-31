package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

final class ZLinkLocalCallbackCauseTest {
    @Test
    void localUserSpotOnCreatePreservesEmptyApplicationException() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("callback-cause")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addSpotFactory(
                        "failing", FailingSpot.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var creation =
                    runtime.spotManager()
                            .getOrCreate("callback-cause", "failing")
                            .submit()
                            .toCompletableFuture();
            var failure =
                    assertThrows(ExecutionException.class, () -> creation.get(5, TimeUnit.SECONDS));
            Throwable cause = failure.getCause();
            if (!(cause instanceof EmptyCallbackFailure)) cause = cause.getCause();
            assertInstanceOf(EmptyCallbackFailure.class, cause);
            assertSame(FailingSpot.ORIGINAL, cause);
            assertEquals("", cause.getMessage());
        }
    }

    public static final class EmptyCallbackFailure extends RuntimeException {
        EmptyCallbackFailure() {
            super("");
        }
    }

    public static final class FailingSpot implements ZLinkSpot<ZLinkActor> {
        static final EmptyCallbackFailure ORIGINAL = new EmptyCallbackFailure();
        private final ZLinkSpotContext context;

        public FailingSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            throw ORIGINAL;
        }
    }
}
