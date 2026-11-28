package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.spots.ZLinkRelocationSettlementFixture.OPEN;
import static systems.zlink.framework.runtime.spots.ZLinkRelocationSettlementFixture.STAGE_DEADLINE;
import static systems.zlink.framework.runtime.spots.ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED;
import static systems.zlink.framework.runtime.spots.ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityRelocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.spots.ZLinkRelocationSettlementFixture.Fixture;

import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;

final class ZLinkCanonicalRelocationPreserveContractTest {
    @Test
    void preparedVersionChangeLeavesSourceClosed() throws Exception {
        var fixture = new Fixture(true);
        fixture.stage();
        publishPreparedRow(fixture);

        var settlement =
                fixture.source
                        .settle(fixture.targetRid, fixture.request.fence(), STAGE_DEADLINE)
                        .toCompletableFuture();
        fixture.nextManagementTurn(settlement);
        assertFalse(
                settlement.isDone(),
                () ->
                        "Prepared must keep source dispatch closed, settlement="
                                + settlement.getNow(null));
        assertEquals(0, fixture.endpoint.published.get());
    }

    @Test
    void targetCommitWinsWhileSourcePreserveIsPending() throws Exception {
        var fixture = new Fixture(true);
        fixture.stage();
        var source = fixture.authority();
        var preserveEntered = new CompletableFuture<Void>();
        var releasePreserve = new CompletableFuture<Void>();
        var conflictObserved = new CompletableFuture<ZLinkAuthorityConflict>();
        fixture.preserveWrite =
                write -> {
                    preserveEntered.complete(null);
                    return releasePreserve
                            .thenCompose(ignored -> write.get())
                            .thenApply(
                                    result -> {
                                        conflictObserved.complete(
                                                assertInstanceOf(
                                                        ZLinkAuthorityConflict.class, result));
                                        return result;
                                    });
                };

        var settlement =
                fixture.source
                        .settle(fixture.targetRid, fixture.request.fence(), Instant.EPOCH)
                        .toCompletableFuture();
        preserveEntered.get(3, TimeUnit.SECONDS);
        assertFalse(settlement.isDone());

        fixture.publish();
        fixture.endpoint.publishedEvent.get(3, TimeUnit.SECONDS);
        releasePreserve.complete(null);

        assertEquals(TARGET_COMMITTED, settlement.get(3, TimeUnit.SECONDS));
        assertTrue(conflictObserved.isDone(), "the source reconciles the losing CAS");
        var committed = fixture.authority();
        assertEquals(fixture.targetOwner.ownerId(), committed.ownerId());
        assertEquals(source.objectGeneration(), committed.objectGeneration());
        assertEquals(source.authorityOwnerGeneration() + 1, committed.authorityOwnerGeneration());
        assertEquals(1, fixture.commits.get());
        assertEquals(1, fixture.endpoint.published.get());
    }

    @Test
    void sourcePreserveWinsBeforeLateCutover() throws Exception {
        var fixture = new Fixture(true);
        fixture.dropCutover.set(true);
        fixture.stage();
        assertThrows(CompletionException.class, fixture::publish);
        var source = fixture.authority();

        var settlement =
                fixture.source
                        .settle(fixture.targetRid, fixture.request.fence(), Instant.EPOCH)
                        .toCompletableFuture();
        assertEquals(SOURCE_PRESERVED, settlement.get(3, TimeUnit.SECONDS));
        var preserved = fixture.authority();
        assertTrue(preserved.relocation().aborted());
        assertNotEquals(source.storeVersion(), preserved.storeVersion());

        fixture.applyToTarget(
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER, fixture.cutover.get());
        fixture.endpoint.abortedEvent.get(3, TimeUnit.SECONDS);
        assertEquals(preserved.storeVersion(), fixture.authority().storeVersion());
        assertEquals(fixture.sourceOwner.ownerId(), fixture.authority().ownerId());
        assertEquals(0, fixture.endpoint.published.get());
    }

    @Test
    void lostPreserveReplyRequiresObservingTheDurableAbort() throws Exception {
        var fixture = new Fixture(true);
        fixture.stage();
        publishPreparedRow(fixture);
        fixture.preserveWrite =
                write ->
                        write.get()
                                .thenCompose(
                                        result -> {
                                            assertInstanceOf(ZLinkAuthorityStored.class, result);
                                            return CompletableFuture.failedFuture(
                                                    new IllegalStateException(
                                                            "Preserve reply lost"));
                                        });

        var settlement =
                fixture.source
                        .settle(fixture.targetRid, fixture.request.fence(), Instant.EPOCH)
                        .toCompletableFuture();
        var retry = fixture.management.poll(3, TimeUnit.SECONDS);
        assertNotNull(retry, "the source schedules a new observation after the lost reply");
        assertTrue(fixture.authority().relocation().aborted());
        assertFalse(settlement.isDone());
        retry.run();

        assertEquals(SOURCE_PRESERVED, settlement.get(3, TimeUnit.SECONDS));
        assertEquals(fixture.sourceOwner.ownerId(), fixture.authority().ownerId());
        assertEquals(0, fixture.endpoint.published.get());
    }

    private static void publishPreparedRow(Fixture fixture) {
        var captured = fixture.authority();
        assertEquals("captured", captured.relocation().phase());

        // A target can publish its Prepared phase without an aggregate transaction marker.
        var prepared = captured.relocation().json();
        prepared.put("phase", "prepared");
        prepared.putObject("target")
                .put("nodeRid", fixture.targetRid.toHex())
                .put(
                        "nodeGeneration",
                        Long.toUnsignedString(fixture.request.targetNodeGeneration()))
                .put("ownerId", fixture.targetOwner.ownerId())
                .put(
                        "ownerLeaseGeneration",
                        Long.toUnsignedString(fixture.targetOwner.leaseGeneration()))
                .put(
                        "attemptGeneration",
                        Long.toUnsignedString(fixture.targetAttemptGeneration.get()));
        assertInstanceOf(
                ZLinkAuthorityStored.class,
                fixture.locations
                        .compareExchange(
                                fixture.authorityKey,
                                new ZLinkAuthorityExpectFound(captured.storeVersion()),
                                ZLinkAuthorityPut.relocation(
                                        captured.payload(), new ZLinkAuthorityRelocation(prepared)),
                                OPEN)
                        .toCompletableFuture()
                        .join());
        var current = fixture.authority();
        assertNotEquals(captured.storeVersion(), current.storeVersion());
        assertEquals(captured.ownerId(), current.ownerId());
        assertEquals(captured.ownerLeaseGeneration(), current.ownerLeaseGeneration());
        assertEquals(captured.objectGeneration(), current.objectGeneration());
        assertEquals(captured.authorityOwnerGeneration(), current.authorityOwnerGeneration());
    }
}
