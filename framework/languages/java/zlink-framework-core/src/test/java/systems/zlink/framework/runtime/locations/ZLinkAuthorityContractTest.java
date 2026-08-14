package systems.zlink.framework.runtime.locations;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectRejectResult;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Instant;
import java.util.Optional;
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
                "getMeshNodeChangeStamp",
                String.class).getReturnType());
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
        ZLinkAuthorityPut put = new ZLinkAuthorityPut(payload);
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
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkObjectRejectResult> reject(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
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
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            throw new AssertionError();
        }

        @Override
        public CompletionStage<ZLinkCreationTerminalReadResult>
            readCreationTerminal(
                ZLinkCreationOperationIdentity operation,
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
