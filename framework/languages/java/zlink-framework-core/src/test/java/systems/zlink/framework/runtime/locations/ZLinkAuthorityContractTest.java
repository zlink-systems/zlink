package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Instant;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMutation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityRestore;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserveResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityReserveResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

final class ZLinkAuthorityContractTest {
    @Test
    void exactAuthorityStoreMethodsArePublic() throws Exception {
        assertEquals(
            CompletionStage.class,
            ZLinkLocationRepository.class.getMethod(
                "read",
                String.class,
                ZLinkStoreCancellation.class).getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkLocationRepository.class.getMethod(
                "compareExchange",
                String.class,
                ZLinkAuthorityExpectation.class,
                ZLinkAuthorityMutation.class,
                ZLinkStoreCancellation.class).getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkLocationRepository.class.getMethod(
                "list",
                String.class,
                Optional.class,
                int.class,
                ZLinkStoreCancellation.class).getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkLocationRepository.class.getMethod(
                "prepareAggregate",
                ZLinkAggregatePrepareRequest.class,
                ZLinkStoreCancellation.class).getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkLocationRepository.class.getMethod(
                "reserveRelocationCapacity",
                ZLinkRelocationCapacityReservationRequest.class,
                ZLinkStoreCancellation.class).getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkLocationRepository.class.getMethod(
                "abortRelocationCapacity",
                ZLinkRelocationCapacityFence.class,
                ZLinkStoreCancellation.class).getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkLocationRepository.class.getMethod(
                "getMeshNodeChangeStamp",
                String.class).getReturnType());
    }

    @Test
    void authorityPutRequiresTheExactOwnerAndCapacityFenceCombination() {
        var owner = new ZLinkLocationOwnerToken("owner", 1);
        var fence = new ZLinkRelocationCapacityFence("capacity");

        ZLinkAuthorityPut relocation = new ZLinkAuthorityPut(
            new byte[0],
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            Optional.of(owner),
            Optional.of(fence));
        assertEquals(fence, relocation.relocationCapacityFence().orElseThrow());

        assertThrows(IllegalArgumentException.class, () ->
            new ZLinkAuthorityPut(
                new byte[0],
                ZLinkAuthorityGenerationTransition.PRESERVE,
                Optional.of(owner),
                Optional.empty()));
        assertThrows(IllegalArgumentException.class, () ->
            new ZLinkAuthorityPut(
                new byte[0],
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                Optional.of(owner),
                Optional.empty()));
        assertEquals(
            2,
            ZLinkAuthorityGenerationTransition.values().length);
        assertArrayEquals(
            new Class<?>[] {ZLinkAuthorityExpectFound.class},
            ZLinkAuthorityExpectation.class.getPermittedSubclasses());
        assertEquals(
            Set.of(
                ZLinkAuthorityPut.class,
                ZLinkAuthorityRestore.class,
                systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete.class),
            Set.of(ZLinkAuthorityMutation.class.getPermittedSubclasses()));
    }

    @Test
    void relocationCapacityRequestRequiresBothDescriptorLifecycles() {
        var descriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.from("node"));
        var source = new ZLinkLocationOwnerToken("source", 1);
        var target = new ZLinkLocationOwnerToken("target", 2);

        var request = new ZLinkRelocationCapacityReservationRequest(
            UUID.randomUUID(),
            "authority",
            "version",
            ZLinkPlacementObjectKind.ACTOR,
            "player",
            descriptor,
            3,
            source,
            descriptor,
            4,
            target,
            ZLinkPlacementCapacityBundle.actor(1));
        assertEquals(3, request.sourceDescriptorLifecycleGeneration());
        assertEquals(4, request.targetDescriptorLifecycleGeneration());
        assertThrows(IllegalArgumentException.class, () ->
            new ZLinkRelocationCapacityReservationRequest(
                UUID.randomUUID(),
                "authority",
                "version",
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                descriptor,
                0,
                source,
                descriptor,
                4,
                target,
                ZLinkPlacementCapacityBundle.actor(1)));
        assertThrows(IllegalArgumentException.class, () ->
            new ZLinkRelocationCapacityReservationRequest(
                UUID.randomUUID(),
                "authority",
                "version",
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                descriptor,
                3,
                source,
                descriptor,
                4,
                target,
                new ZLinkPlacementCapacityBundle(
                    0, 0, Optional.empty())));
    }

    @Test
    void authorityPayloadRecordsDefensivelyCopyBytes() {
        byte[] payload = new byte[] {1, 2, 3};
        ZLinkPlacementAllocation allocation =
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.from("node")),
                7,
                ZLinkPlacementCapacityBundle.actor(1));
        ZLinkAuthoritySnapshot snapshot = new ZLinkAuthoritySnapshot(
            "v1",
            payload,
            11,
            12,
            "owner",
            13,
            allocation,
            Instant.EPOCH);
        ZLinkAuthorityPut put = new ZLinkAuthorityPut(
            payload,
            systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition.PRESERVE,
            Optional.empty(),
            Optional.empty());
        ZLinkAuthorityStored stored = new ZLinkAuthorityStored(
            "v2",
            payload,
            11,
            12,
            "owner",
            13,
            allocation,
            Instant.EPOCH);

        payload[0] = 9;
        assertArrayEquals(new byte[] {1, 2, 3}, snapshot.payload());
        assertArrayEquals(new byte[] {1, 2, 3}, put.payload());
        assertArrayEquals(new byte[] {1, 2, 3}, stored.payload());
        snapshot.payload()[1] = 9;
        assertArrayEquals(new byte[] {1, 2, 3}, snapshot.payload());
    }

    @Test
    void registeredLocationCapabilityExposesTheSameAuthorityProvider() {
        ZLinkLocationRepository authority = new ContractAuthorityStore();
        ZLinkRegisteredLocationStores stores =
            ZLinkRegisteredLocationStores.fromUnified(authority);
        ZLinkHandlerActivator.MutableServices services =
            ZLinkHandlerActivator.services();

        stores.addTo(services);

        assertSame(authority, services.create(ZLinkLocationRepository.class));
    }

    private static final class ContractAuthorityStore
        extends ZLinkLocationStoreTestAdapter {
        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
            String key,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key,
            ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkAuthorityScanResult> list(
            String prefix,
            Optional<ZLinkAuthorityScanCursor> cursor,
            int limit,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkObjectRejectResult> reject(
            ZLinkObjectReservation reservation,
            systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalReadResult>
            readCreationTerminal(
                systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity operation,
                ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkRelocationCapacityReserveResult>
            reserveRelocationCapacity(
                ZLinkRelocationCapacityReservationRequest request,
                ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkRelocationCapacityAbortResult>
            abortRelocationCapacity(
                ZLinkRelocationCapacityFence fence,
                ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }
    }
}
