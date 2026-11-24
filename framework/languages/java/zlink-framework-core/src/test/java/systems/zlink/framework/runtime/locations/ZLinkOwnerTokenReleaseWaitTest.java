package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

import java.time.Clock;
import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Supplier;

final class ZLinkOwnerTokenReleaseWaitTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void sharesAcrossRowsAndSeparatesGenerations(boolean inMemory) {
        var now = new AtomicLong();
        var provider = new ZLinkInMemoryProviderLocationStore();
        var margin = Duration.ofNanos(5);
        Supplier<ZLinkLocationRepository> repository =
                () ->
                        inMemory
                                ? new ZLinkInMemoryLocationStore(
                                        Clock.systemUTC(), now::get, margin)
                                : new ZLinkProviderLocationRepository(provider, now::get, margin);

        var store = repository.get();
        var target = seed(store);
        assertWaiting(store, target, "first");
        now.set(14);
        assertWaiting(store, target, "first");
        now.set(15);
        assertInstanceOf(ZLinkObjectReserved.class, reserve(store, target, "first"));
        assertInstanceOf(ZLinkObjectReserved.class, reserve(store, target, "second"));
        assertWaiting(store, target, "new-generation");

        var restarted = repository.get();
        if (inMemory) seed(restarted);
        assertWaiting(restarted, target, "untouched");
        now.set(30);
        assertInstanceOf(ZLinkObjectReserved.class, reserve(store, target, "new-generation"));
        assertInstanceOf(ZLinkObjectReserved.class, reserve(restarted, target, "untouched"));
    }

    private static ZLinkMeshNodeDescriptor seed(ZLinkLocationRepository store) {
        var old = publish(store, "release-owner", "first-node");
        for (var id : new String[] {"first", "second", "untouched"}) createReady(store, old, id);
        assertEquals(
                ZLinkOwnerLeaseReleaseResult.RELEASED,
                store.releaseOwnerLease(token(old)).toCompletableFuture().join());

        var next = publish(store, old.ownerId(), "next-node");
        assertEquals(old.leaseGeneration() + 1, next.leaseGeneration());
        createReady(store, next, "new-generation");
        assertEquals(
                ZLinkOwnerLeaseReleaseResult.RELEASED,
                store.releaseOwnerLease(token(next)).toCompletableFuture().join());
        return publish(store, "release-target", "target-node");
    }

    private static ZLinkMeshNodeDescriptor publish(
            ZLinkLocationRepository store, String ownerId, String rid) {
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                store.claimOwnerLease(ownerId, Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var descriptor =
                ZLinkLocationDescriptorFixture.descriptor(
                        owner, rid, ZLinkPlacementObjectKind.USER_SPOT, 10);
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .join()
                        .status());
        return descriptor;
    }

    private static void createReady(
            ZLinkLocationRepository store, ZLinkMeshNodeDescriptor target, String id) {
        var reservation = assertInstanceOf(ZLinkObjectReserved.class, reserve(store, target, id));
        var ready =
                new ZLinkActorAuthorityPayloadCodec()
                        .encode(
                                ZLinkActorAuthorityPayloadCodec.State.READY,
                                "player",
                                id,
                                target.entrySpotId().orElseThrow(),
                                1,
                                1,
                                target.ownerId(),
                                target.leaseGeneration(),
                                target.meshName(),
                                target.rid(),
                                target.lifecycleGeneration());
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(reservation.reservation(), ready, () -> false)
                        .toCompletableFuture()
                        .join());
    }

    private static ZLinkObjectReserveResult reserve(
            ZLinkLocationRepository store, ZLinkMeshNodeDescriptor target, String id) {
        return store.reserve(
                        new ZLinkObjectReservationRequest(
                                ZLinkPlacementObjectKind.ACTOR,
                                ZLinkAuthorityKeyCodec.actor(id),
                                "player",
                                "intent",
                                new byte[32],
                                1,
                                new ZLinkMeshNodeDescriptorKey(target.meshName(), target.rid()),
                                target.lifecycleGeneration(),
                                token(target),
                                new byte[] {1},
                                ZLinkPlacementCapacityBundle.actor(1)),
                        () -> false)
                .toCompletableFuture()
                .join();
    }

    private static void assertWaiting(
            ZLinkLocationRepository store, ZLinkMeshNodeDescriptor target, String id) {
        var failure = assertThrows(RuntimeException.class, () -> reserve(store, target, id));
        var cause = failure instanceof CompletionException ? failure.getCause() : failure;
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, cause).kind());
    }

    private static ZLinkLocationOwnerToken token(ZLinkMeshNodeDescriptor descriptor) {
        return new ZLinkLocationOwnerToken(descriptor.ownerId(), descriptor.leaseGeneration());
    }
}
