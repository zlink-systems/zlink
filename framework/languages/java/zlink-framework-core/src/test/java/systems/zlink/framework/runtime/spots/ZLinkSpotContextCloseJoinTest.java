package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkSpotContextCloseJoinTest {
    private static final long WAIT_SECONDS = 10;
    private static final AtomicReference<ZLinkSpotContext> SPOT_CONTEXT = new AtomicReference<>();

    @Test
    void contextCloseJoinsAfterActivationDetachWithTheSharedSuccessOrFailure() throws Exception {
        for (boolean failRelease : new boolean[] {false, true}) {
            String spotId = "context-close-join-" + UUID.randomUUID();
            var store = new CloseStore(new ZLinkInMemoryLocationStore(), spotId);
            var options = new DefaultZLinkFrameworkOptions();
            options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
            options.addLocationStore(store);
            options.addRouteMesh("context-close-join")
                    .listen("inproc://" + spotId)
                    .setRoutingId(RoutingId.from(spotId))
                    .objects()
                    .server()
                    .addSpotFactory("room", RoomSpot.class, factory -> factory.disableRelocation());

            try (var runtime =
                    ZLinkFrameworkRuntimeTestAccess.start(
                            options, new ZLinkJavaBackendAdapterFactory())) {
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertTrue(
                        ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS));
                runtime.spotManager()
                        .getOrCreate(spotId, "room")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);

                var host = ZLinkFrameworkRuntimeTestAccess.spots(runtime);
                var context = SPOT_CONTEXT.get();
                assertNotNull(context);
                var detached = new CompletableFuture<Void>();
                var releaseDetached = new CompletableFuture<Void>();
                host.setCountChanged(
                        () -> {
                            if (host.spotLifecycle().spotActivationFor(spotId) == null
                                    && detached.complete(null)) {
                                releaseDetached.join();
                            }
                        });
                if (failRelease) store.failNextAuthorityDelete.set(true);

                var owner = context.close().toCompletableFuture();
                try {
                    detached.get(WAIT_SECONDS, TimeUnit.SECONDS);
                    assertFalse(
                            owner.isDone(), "Close must remain pending after activation detach");
                    var joined = context.close().toCompletableFuture();
                    assertFalse(joined.isDone(), "joined Close must await the shared terminal");
                    var cancelled = context.close().toCompletableFuture();
                    assertTrue(cancelled.cancel(false));
                    assertFalse(
                            owner.isDone() || joined.isDone(),
                            "a joined caller's cancel must not settle the shared Close");
                    releaseDetached.complete(null);
                    if (failRelease) {
                        var ownerFailure =
                                assertThrows(
                                        ExecutionException.class,
                                        () -> owner.get(WAIT_SECONDS, TimeUnit.SECONDS));
                        var joinedFailure =
                                assertThrows(
                                        ExecutionException.class,
                                        () -> joined.get(WAIT_SECONDS, TimeUnit.SECONDS));
                        assertSame(ownerFailure.getCause(), joinedFailure.getCause());
                    } else {
                        assertTrue(owner.get(WAIT_SECONDS, TimeUnit.SECONDS));
                        assertTrue(joined.get(WAIT_SECONDS, TimeUnit.SECONDS));
                    }
                } finally {
                    releaseDetached.complete(null);
                }
            }
        }
    }

    private static final class CloseStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner;
        private final String spotId;
        private final AtomicBoolean failNextAuthorityDelete = new AtomicBoolean();

        CloseStore(ZLinkLocationStore inner, String spotId) {
            this.inner = inner;
            this.spotId = spotId;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean authorityDelete =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStoreDelete delete
                                                    && delete.key()
                                                            .value()
                                                            .startsWith("authority\0")
                                                    && delete.key()
                                                            .value()
                                                            .endsWith("\0" + spotId));
            if (authorityDelete && failNextAuthorityDelete.compareAndSet(true, false)) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("injected authority release failure"));
            }
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    public static final class RoomSpot implements ZLinkSpot<TestActor> {
        private final ZLinkSpotContext context;

        public RoomSpot(ZLinkSpotContext context) {
            this.context = context;
            SPOT_CONTEXT.set(context);
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TestActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TestActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class TestActor implements ZLinkActor {
        @Override
        public ZLinkActorContext context() {
            return null;
        }
    }
}
