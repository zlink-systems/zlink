package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkAggregateInventoryCleanupTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void interruptedCleanupLeavesRootAndTerminalRecordUntilChildrenAreGone(boolean committed) {
        var fixture = new Fixture();
        var record = fixture.seed(committed ? (byte) 2 : (byte) 3);
        fixture.interruptAtRoot.set(true);

        assertThrows(CompletionException.class, () -> fixture.cleanup(committed, record));
        assertTrue(fixture.isPresent(fixture.root));
        assertTrue(fixture.isPresent(fixture.marker));
        assertEquals(3, fixture.deleted.size());
        assertTrue(
                fixture.deleted.stream()
                        .noneMatch(key -> key.equals(fixture.root) || key.equals(fixture.marker)));

        fixture.cleanup(committed, record);

        assertEquals(List.of(fixture.root, fixture.marker), fixture.deleted.subList(3, 5));
        assertTrue(fixture.deleted.stream().noneMatch(fixture::isPresent));
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void terminalRecordWithoutRootCanFinishCleanup(boolean committed) {
        var fixture = new Fixture();
        var record = fixture.seed(committed ? (byte) 2 : (byte) 3);
        fixture.inventory
                .delete(fixture.fence, OPEN::isCancellationRequested)
                .toCompletableFuture()
                .join();

        assertTrue(
                fixture.repository
                        .readAggregateProgress(fixture.fence, OPEN)
                        .toCompletableFuture()
                        .join()
                        .isEmpty());
        if (committed) {
            assertEquals(
                    ZLinkAggregateCommitResult.ALREADY_COMMITTED,
                    fixture.repository
                            .commitAggregate(fixture.fence, OPEN)
                            .toCompletableFuture()
                            .join());
        }
        fixture.cleanup(committed, record);
        assertEquals(fixture.marker, fixture.deleted.getLast());
        assertTrue(fixture.deleted.stream().noneMatch(fixture::isPresent));
    }

    @Test
    void preparedRecordWithMissingPageIsDataLost() {
        var fixture = new Fixture();
        fixture.seed((byte) 1);
        var page = new ZLinkStoreKey(fixture.root.value().replaceFirst("root$", "page:0:0"));
        fixture.provider
                .write(
                        new ZLinkStoreWriteRequest(List.of(), List.of(new ZLinkStoreDelete(page))),
                        OPEN::isCancellationRequested)
                .toCompletableFuture()
                .join();
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                fixture.repository
                                        .commitAggregate(fixture.fence, OPEN)
                                        .toCompletableFuture()
                                        .join());
        assertInstanceOf(
                ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                failure.getCause());
    }

    private static final class Fixture {
        final ZLinkInMemoryProviderLocationStore provider =
                new ZLinkInMemoryProviderLocationStore();
        final List<ZLinkStoreKey> deleted = new ArrayList<>();
        final AtomicBoolean interruptAtRoot = new AtomicBoolean();
        final ZLinkAggregateFence fence = ZLinkAggregateFence.forRelocation(UUID.randomUUID());
        final ZLinkStoreKey root = ZLinkAggregateInventoryStore.rootKey(fence);
        final ZLinkStoreKey marker = ZLinkProviderAuthorityRepository.aggregateKey(fence);
        final ZLinkLocationStore recording =
                (ZLinkLocationStore)
                        Proxy.newProxyInstance(
                                ZLinkLocationStore.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationStore.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("write")) {
                                        var write = (ZLinkStoreWriteRequest) args[0];
                                        var deletes =
                                                write.mutations().stream()
                                                        .filter(ZLinkStoreDelete.class::isInstance)
                                                        .map(
                                                                mutation ->
                                                                        ((ZLinkStoreDelete)
                                                                                        mutation)
                                                                                .key())
                                                        .toList();
                                        if (deletes.contains(root)
                                                && interruptAtRoot.compareAndSet(true, false)) {
                                            return CompletableFuture.failedFuture(
                                                    new IllegalStateException("interrupted"));
                                        }
                                        deleted.addAll(deletes);
                                    }
                                    try {
                                        return method.invoke(provider, args);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
        final ZLinkProviderLocationRepository repository =
                new ZLinkProviderLocationRepository(recording);
        final ZLinkAggregateInventoryStore inventory = new ZLinkAggregateInventoryStore(recording);

        ZLinkStoreReadFound seed(byte state) {
            var owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    repository
                                            .claimOwnerLease("target", Duration.ofMinutes(1))
                                            .toCompletableFuture()
                                            .join())
                            .token();
            var participants =
                    List.of(
                            new ZLinkAggregateParticipant(
                                    ZLinkAuthorityKeyCodec.actor("alpha"),
                                    1,
                                    1,
                                    "version-1",
                                    ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                    new byte[] {42},
                                    new byte[] {'[', ']'}));
            var request =
                    new ZLinkAggregatePrepareRequest(
                            fence.aggregateId(),
                            fence.aggregateGeneration(),
                            participants,
                            new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.from("target")),
                            1,
                            ZLinkPlacementCapacityBundle.actor(1),
                            owner);
            inventory
                    .store(fence, participants, OPEN::isCancellationRequested)
                    .toCompletableFuture()
                    .join();
            provider.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new ZLinkStorePut(
                                                    marker,
                                                    ZLinkProviderAuthorityRepository
                                                            .encodeAggregate(state, request),
                                                    null))),
                            OPEN::isCancellationRequested)
                    .toCompletableFuture()
                    .join();
            return assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    provider.read(marker, OPEN::isCancellationRequested)
                            .toCompletableFuture()
                            .join());
        }

        boolean isPresent(ZLinkStoreKey key) {
            return provider.read(key, OPEN::isCancellationRequested).toCompletableFuture().join()
                    instanceof ZLinkStoreReadFound;
        }

        void cleanup(boolean committed, ZLinkStoreReadFound record) {
            if (committed) {
                assertTrue(
                        repository
                                .removeAggregateProgress(
                                        fence, record.value().version().value(), OPEN)
                                .toCompletableFuture()
                                .join());
            } else {
                assertEquals(
                        ZLinkAggregateAbortResult.ALREADY_ABORTED,
                        repository.abortAggregate(fence, OPEN).toCompletableFuture().join());
            }
        }
    }
}
