package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class InstanceSpotHostRelocationIntegrationTest extends InstanceSpotHostRelocationFixture {
    @Test
    void hostMovesItsOnlyInstanceSpotToTargetWithTheSameGeneration() throws Exception {
        try (var source = start(SOURCE);
                var caller = start(CLIENT)) {
            var client = activateFrom(caller);
            var original = awaitReadyAuthority();
            assertEquals(SOURCE, original.allocation().descriptor().rid());

            try (var target = start(TARGET)) {
                awaitPeer(source, TARGET);
                awaitPeer(caller, TARGET);
                assertRelocated(source);
                var moved = authority();
                assertEquals(TARGET, moved.allocation().descriptor().rid());
                assertEquals(original.objectGeneration(), moved.objectGeneration());
                assertEquals(original.allocation().objectKind(), moved.allocation().objectKind());
                assertEquals(original.allocation().stableType(), moved.allocation().stableType());
                assertTrue(moved.authorityOwnerGeneration() > original.authorityOwnerGeneration());
                assertEquals(TARGET, awaitReadyAuthority().allocation().descriptor().rid());
                assertEquals("room:state-before-move", readState(client));
            }
        }
    }

    @Test
    void committedCloseExcludesTheInstanceFromHostRelocation() throws Exception {
        Room.releaseClose = new CompletableFuture<>();
        try (var source = start(SOURCE);
                var caller = start(CLIENT)) {
            activateFrom(caller);
            var original = awaitReadyAuthority();
            var close = await(Room.sourceContext).close();
            try {
                await(Room.closingEntered);
                assertRelocated(source);
                var closing = authority();
                assertEquals(SOURCE, closing.allocation().descriptor().rid());
                assertEquals(original.objectGeneration(), closing.objectGeneration());
                assertEquals(
                        ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
                        new ZLinkServiceAuthorityPayloadCodec()
                                .decode(closing.payload())
                                .orElseThrow()
                                .state());
            } finally {
                Room.releaseClose.complete(null);
            }
            assertTrue(await(close));
            assertInstanceOf(
                    ZLinkAuthorityMissing.class,
                    await(authorityStore.read(AUTHORITY, () -> false)));
        }
    }

    @Test
    void storeCommittedCloseSettlesWithoutLocalCloseNotification() throws Exception {
        try (var source = start(SOURCE);
                var caller = start(CLIENT)) {
            activateFrom(caller);
            var original = awaitReadyAuthority();
            try (var target = start(TARGET)) {
                awaitPeer(source, TARGET);
                var closing =
                        new ZLinkServiceAuthorityPayloadCodec()
                                .encodeInstance(
                                        ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
                                        SPOT_TYPE,
                                        SPOT_ID,
                                        original.ownerId(),
                                        original.ownerLeaseGeneration(),
                                        MESH,
                                        SOURCE,
                                        original.allocation().descriptorLifecycleGeneration());
                var committed =
                        assertInstanceOf(
                                ZLinkAuthorityStored.class,
                                await(
                                        authorityStore.compareExchange(
                                                AUTHORITY,
                                                new ZLinkAuthorityExpectFound(
                                                        original.storeVersion()),
                                                new ZLinkAuthorityPut(closing),
                                                () -> false)));
                // The provider can expose Closing before the source sees the Close terminal.
                assertRelocated(source);
                assertEquals(committed.storeVersion(), authority().storeVersion());
                assertFalse(Room.restoreEntered.isDone());
                assertTrue(await(await(Room.sourceContext).close()));
                assertInstanceOf(
                        ZLinkAuthorityMissing.class,
                        await(authorityStore.read(AUTHORITY, () -> false)));
            }
        }
    }

    @Test
    void closeQueuedDuringRestoreDoesNotCloseTheRelocatedInstance() throws Exception {
        Room.releaseRestore = new CompletableFuture<>();
        try (var source = start(SOURCE);
                var caller = start(CLIENT)) {
            var client = activateFrom(caller);
            var original = awaitReadyAuthority();
            try (var target = start(TARGET)) {
                awaitPeer(source, TARGET);
                awaitPeer(caller, TARGET);
                var move = relocate(source);
                CompletionStage<Boolean> close;
                try {
                    await(Room.restoreEntered);
                    assertEquals(SOURCE, authority().allocation().descriptor().rid());
                    close = await(Room.sourceContext).close();
                    assertFalse(close.toCompletableFuture().isDone());
                } finally {
                    Room.releaseRestore.complete(null);
                }
                var result = await(move);
                assertEquals(
                        ZLinkFrameworkRelocationOutcome.RELOCATED,
                        result.outcome(),
                        result::toString);
                assertFalse(await(close));
                assertFalse(Room.closingEntered.isDone());
                var moved = awaitReadyAuthority();
                assertEquals(TARGET, moved.allocation().descriptor().rid());
                assertEquals(original.objectGeneration(), moved.objectGeneration());
                assertEquals("room:state-before-move", readState(client));
            }
        }
    }

    private static String readState(ZLinkEntrySpotContext client) throws Exception {
        return await(
                client.outbound().requestToSpot(SPOT_ID, "").inMesh(MESH).submit(String.class));
    }

    private void assertRelocated(ZLinkFrameworkRuntime source) throws Exception {
        var result = await(relocate(source));
        assertEquals(ZLinkFrameworkRelocationOutcome.RELOCATED, result.outcome(), result::toString);
    }
}
