package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkActorCreationReplacementTest {
    private static final String ACTOR_ID = "player-1";
    private static final RoutingId TARGET_RID = RoutingId.from("replacement-target");

    @Test
    void replacementOwnerUnavailableRemainsRetryable() {
        var store = new ReplacementStore();
        try (var original = startTarget(store)) {
            assertInstanceOf(ZLinkActorCreateResult.Created.class, getOrCreate(original));
        }

        try (var replacement = startTarget(store)) {
            store.failNextActorWrite.set(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            "replacement owner is unavailable"));
            var failed = assertThrows(CompletionException.class, () -> getOrCreate(replacement));
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failed.getCause()).kind());
            String endpoint =
                    replacement.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game").endpoint();
            try (var requester = startRequester(store, endpoint)) {
                awaitPeerReady(requester);
                var created =
                        assertInstanceOf(
                                ZLinkActorCreateResult.Created.class, getOrCreate(requester));
                var existing =
                        assertInstanceOf(
                                ZLinkActorCreateResult.Existing.class, getOrCreate(requester));
                assertEquals(created.actor(), existing.actor());
            }
        }
    }

    private static ZLinkFrameworkRuntime startTarget(ZLinkLocationStore store) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var mesh = options.addRouteMesh("game");
        mesh.listen("tcp://127.0.0.1:0").setRoutingId(TARGET_RID);
        mesh.objects()
                .server()
                .addEntrySpot(ActorManagerTest.ActorCreationEntry.class)
                .addActorFactory(
                        "player",
                        ActorManagerTest.PlayerActor.class,
                        ActorManagerTest.PlayerActorFactory.class,
                        factory -> factory.disableRelocation());
        return RuntimeTestSupport.startFixedRidFramework(
                options, new ZLinkJavaBackendAdapterFactory());
    }

    private static ZLinkFrameworkRuntime startRequester(ZLinkLocationStore store, String endpoint) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var mesh = options.addRouteMesh("game");
        mesh.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from("replacement-requester"));
        mesh.objects().client();
        mesh.peerConnections().connect(endpoint);
        return RuntimeTestSupport.startFixedRidFramework(
                options, new ZLinkJavaBackendAdapterFactory());
    }

    private static void awaitPeerReady(ZLinkFrameworkRuntime requester) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        do {
            if (requester.routeMeshRuntime().snapshot("game").peers().stream()
                    .anyMatch(
                            peer ->
                                    peer.nodeRid().equals(TARGET_RID)
                                            && peer.state() == ZLinkPeerState.READY)) {
                return;
            }
            Thread.onSpinWait();
        } while (System.nanoTime() < deadline);
        fail("Replacement peer did not reach READY");
    }

    private static ZLinkActorCreateResult getOrCreate(ZLinkFrameworkRuntime runtime) {
        return runtime.actorManager()
                .getOrCreate(ACTOR_ID, "player")
                .submit()
                .toCompletableFuture()
                .orTimeout(10, TimeUnit.SECONDS)
                .join();
    }

    private static final class ReplacementStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner = new ZLinkInMemoryLocationStore();
        private final AtomicReference<ZLinkFrameworkException> failNextActorWrite =
                new AtomicReference<>();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (request.mutations().stream()
                    .anyMatch(
                            mutation ->
                                    mutation instanceof ZLinkStorePut put
                                            && put.key()
                                                    .value()
                                                    .startsWith("creation-terminal\0"))) {
                var failure = failNextActorWrite.getAndSet(null);
                if (failure != null) return CompletableFuture.failedFuture(failure);
            }
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }
}
