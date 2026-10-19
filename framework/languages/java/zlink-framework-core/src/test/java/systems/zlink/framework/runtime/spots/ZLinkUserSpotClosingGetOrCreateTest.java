package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateState;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

final class ZLinkUserSpotClosingGetOrCreateTest {
    private static CompletableFuture<Void> entered;
    private static CompletableFuture<Void> release;

    @Test
    void closingCompletesBeforeNewIncarnationIsCreated() throws Exception {
        exercise(false);
    }

    @Test
    void closingConsumesTheGetOrCreateDeadline() throws Exception {
        exercise(true);
    }

    private void exercise(boolean expire) throws Exception {
        entered = new CompletableFuture<>();
        release = new CompletableFuture<>();
        var options = new DefaultZLinkFrameworkOptions();
        var store = new DeleteResponseStore(!expire);
        options.addLocationStore(store);
        options.addRouteMesh("closing-create")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addSpotFactory("room", Room.class, factory -> factory.disableRelocation())
                .addSpotFactory("other", Other.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var manager = runtime.spotManager();
            var original =
                    manager.getOrCreate("closing-room", "room")
                            .inMesh("closing-create")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            var close = manager.close(original.spot()).toCompletableFuture();
            try {
                entered.get(5, TimeUnit.SECONDS);
                var mismatch =
                        manager.getOrCreate("closing-room", "other")
                                .inMesh("closing-create")
                                .submit()
                                .toCompletableFuture();
                var mismatchFailure =
                        assertThrows(
                                ExecutionException.class, () -> mismatch.get(5, TimeUnit.SECONDS));
                assertEquals(
                        ZLinkFrameworkErrorKind.TYPE_MISMATCH,
                        assertInstanceOf(ZLinkFrameworkException.class, mismatchFailure.getCause())
                                .kind());
                var pending =
                        manager.getOrCreate("closing-room", "room")
                                .inMesh("closing-create")
                                .timeout(expire ? Duration.ofMillis(100) : Duration.ofSeconds(5))
                                .submit()
                                .toCompletableFuture();
                if (expire) {
                    var failure =
                            assertThrows(
                                    ExecutionException.class,
                                    () -> pending.get(5, TimeUnit.SECONDS));
                    assertEquals(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                    .kind());
                } else {
                    assertFalse(pending.isDone());
                    release.complete(null);
                    store.deleted.get(5, TimeUnit.SECONDS);
                    assertFalse(close.isDone());
                    var created = pending.get(5, TimeUnit.SECONDS);
                    assertEquals(ZLinkSpotCreateState.CREATED, created.state());
                    assertNotEquals(
                            original.spot().objectGeneration(), created.spot().objectGeneration());
                }
            } finally {
                release.complete(null);
                store.releaseResponse.complete(null);
                assertTrue(close.get(5, TimeUnit.SECONDS));
            }
        }
    }

    private static final class DeleteResponseStore implements ZLinkLocationStore {
        private final ZLinkInMemoryLocationStore inner = new ZLinkInMemoryLocationStore();
        private final boolean hold;
        final CompletableFuture<Void> deleted = new CompletableFuture<>();
        final CompletableFuture<Void> releaseResponse = new CompletableFuture<>();

        DeleteResponseStore(boolean hold) {
            this.hold = hold;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return inner.write(request, cancellation)
                    .thenCompose(
                            result -> {
                                if (hold
                                        && result instanceof ZLinkStoreWriteApplied
                                        && request.mutations().stream()
                                                .anyMatch(
                                                        mutation ->
                                                                mutation
                                                                                instanceof
                                                                                ZLinkStoreDelete
                                                                                        deletion
                                                                        && deletion.key()
                                                                                .value()
                                                                                .startsWith(
                                                                                        "authority\0")
                                                                        && deletion.key()
                                                                                .value()
                                                                                .endsWith(
                                                                                        "\0closing-room"))) {
                                    deleted.complete(null);
                                    return releaseResponse.thenApply(ignored -> result);
                                }
                                return CompletableFuture.completedFuture(result);
                            });
        }
    }

    public static class Room implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onClosing() {
            entered.complete(null);
            return release;
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

    public static final class Other extends Room {
        public Other(ZLinkSpotContext context) {
            super(context);
        }
    }
}
