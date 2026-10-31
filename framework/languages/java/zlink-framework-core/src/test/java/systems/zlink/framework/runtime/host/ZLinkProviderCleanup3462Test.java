package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.*;

import java.time.*;
import java.util.*;
import java.util.concurrent.*;

final class ZLinkProviderCleanup3462Test {
    private static final Instant NOW = Instant.parse("2026-07-03T00:00:00Z");
    private static final RoutingId NODE_A = RoutingId.from(new byte[] {1});

    @Test
    void descriptorReadCompletingAfterDeadlineStartsNoWrite() throws Exception {
        descriptorDeadline(true);
    }

    @Test
    void descriptorReadCompletingBeforeDeadlineRemovesDescriptor() throws Exception {
        descriptorDeadline(false);
    }

    @Test
    void reservedSweepReturnsCapacityInSameBatch() throws Exception {
        sweep(false);
    }

    @Test
    void activeSweepReturnsCapacityInSameBatch() throws Exception {
        sweep(true);
    }

    private static void descriptorDeadline(boolean expire) throws Exception {
        var provider = new DelayedStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                repository
                                        .claimOwnerLease("deadline-owner", Duration.ofSeconds(30))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        repository
                .updateMeshNode(
                        meshNodeDescriptor(owner, NODE_A, 7, 1, "tcp://127.0.0.1:7000", "entry"),
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        Instant deadline = Instant.now().plusMillis(expire ? 20 : 30000);
        provider.armed = true;
        var shutdown = new ZLinkFrameworkShutdown(deadline);
        shutdown.deferStage(
                "descriptor_remove",
                () ->
                        repository
                                .removeMeshNode(
                                        new ZLinkMeshNodeDescriptorKey("mesh", NODE_A),
                                        owner,
                                        () -> !Instant.now().isBefore(deadline))
                                .thenApply(v -> null));
        var stopping = shutdown.closeAsync().toCompletableFuture();
        assertTrue(provider.readEntered);
        assertFalse(stopping.isDone());
        if (expire) {
            assertThrows(TimeoutException.class, () -> stopping.get(30, TimeUnit.MILLISECONDS));
            assertFalse(Instant.now().isBefore(deadline));
        }
        provider.readGate.complete(null);
        if (expire) assertThrows(CompletionException.class, stopping::join);
        else stopping.join();
        assertEquals(expire ? 0 : 1, provider.writes);
        provider.armed = false;
        assertEquals(
                expire ? 1 : 0,
                repository
                        .listMeshNodes("mesh", ZLinkPageRequest.firstPage())
                        .toCompletableFuture()
                        .get()
                        .items()
                        .size());
    }

    private static final class DelayedStore implements ZLinkLocationStore {
        final ZLinkInMemoryProviderLocationStore inner = new ZLinkInMemoryProviderLocationStore();
        final CompletableFuture<Void> readGate = new CompletableFuture<>();
        boolean armed, readEntered;
        int writes;

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            var result = inner.read(key, cancellation);
            if (!armed) return result;
            readEntered = true;
            return readGate.thenCompose(ignored -> result);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (armed) writes++;
            return inner.write(request, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    private static void sweep(boolean active) throws Exception {
        ZLinkLocationRepository store =
                new ZLinkProviderLocationRepository(
                        new ZLinkInMemoryProviderLocationStore(Clock.fixed(NOW, ZoneOffset.UTC)));
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                store.claimOwnerLease("cleanup-owner", Duration.ofSeconds(30))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var other =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                store.claimOwnerLease("other-owner", Duration.ofSeconds(30))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        for (var token : List.of(owner, other)) {
            var rid = token.equals(owner) ? NODE_A : RoutingId.from(new byte[] {2});
            var descriptor =
                    meshNodeDescriptor(token, rid, 7, 1, "tcp://127.0.0.1:7000", token.ownerId());
            store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture()
                    .get();
            String key = ZLinkAuthorityKeyCodec.actor(token.ownerId());
            var reserved =
                    assertInstanceOf(
                            ZLinkObjectReserved.class,
                            store.reserve(
                                            new ZLinkObjectReservationRequest(
                                                    ZLinkPlacementObjectKind.ACTOR,
                                                    key,
                                                    "player",
                                                    "root",
                                                    new byte[32],
                                                    32,
                                                    new ZLinkMeshNodeDescriptorKey("mesh", rid),
                                                    7,
                                                    token,
                                                    new byte[] {1},
                                                    ZLinkPlacementCapacityBundle.actor(1)),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());
            if (active)
                assertEquals(
                        ZLinkObjectCommitResult.COMMITTED,
                        store.commit(reserved.reservation(), new byte[] {2}, () -> false)
                                .toCompletableFuture()
                                .get());
        }
        var before =
                store
                        .listMeshNodes("mesh", ZLinkPageRequest.firstPage())
                        .toCompletableFuture()
                        .get()
                        .items()
                        .stream()
                        .filter(d -> d.ownerId().equals(owner.ownerId()))
                        .findFirst()
                        .orElseThrow()
                        .capacity()
                        .actors();
        assertEquals(1L, active ? before.active() : before.reserved());
        assertEquals(1L, store.removeAllByOwner(owner).toCompletableFuture().get());
        var remainingCapacity =
                store
                        .listMeshNodes("mesh", ZLinkPageRequest.firstPage())
                        .toCompletableFuture()
                        .get()
                        .items()
                        .stream()
                        .filter(d -> d.ownerId().equals(owner.ownerId()))
                        .findFirst()
                        .orElseThrow()
                        .capacity()
                        .actors();

        assertEquals(0L, remainingCapacity.reserved());
        assertEquals(0L, remainingCapacity.active());
        assertEquals(
                2,
                store.listMeshNodes("mesh", ZLinkPageRequest.firstPage())
                        .toCompletableFuture()
                        .get()
                        .items()
                        .size());
        assertInstanceOf(
                systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing.class,
                store.read(ZLinkAuthorityKeyCodec.actor(owner.ownerId()), () -> false)
                        .toCompletableFuture()
                        .get());
        assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(ZLinkAuthorityKeyCodec.actor(other.ownerId()), () -> false)
                        .toCompletableFuture()
                        .get());
        assertInstanceOf(
                systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound.class,
                store.readOwnerLease(owner.ownerId()).toCompletableFuture().get());
        assertEquals(0L, store.removeAllByOwner(owner).toCompletableFuture().get());
        var otherCapacity =
                store
                        .listMeshNodes("mesh", ZLinkPageRequest.firstPage())
                        .toCompletableFuture()
                        .get()
                        .items()
                        .stream()
                        .filter(d -> d.ownerId().equals(other.ownerId()))
                        .findFirst()
                        .orElseThrow()
                        .capacity()
                        .actors();
        assertEquals(1L, active ? otherCapacity.active() : otherCapacity.reserved());
    }

    private static ZLinkMeshNodeDescriptor meshNodeDescriptor(
            ZLinkLocationOwnerToken owner,
            RoutingId rid,
            long lifecycleGeneration,
            long revision,
            String endpoint,
            String entrySpotId) {
        return new ZLinkMeshNodeDescriptor(
                "mesh",
                rid,
                lifecycleGeneration,
                revision,
                endpoint,
                Map.of("game", 100),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                "player",
                                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                true,
                                0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of(entrySpotId),
                100,
                actorCapacity(0, 0, 8),
                new ZLinkActivationConcurrency(0, 128),
                Optional.empty(),
                systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                NOW);
    }

    private static ZLinkPlacementCapacity actorCapacity(int active, int reserved, int limit) {
        return new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(active, reserved, limit),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of());
    }
}
