package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

final class ZLinkProviderCreationDeadlineTest {
    @Test
    void cancelledTransitionDoesNotRebuildOrPublishReady() {
        var fixture = new ZLinkProviderCreationTerminalTest.Fixture();
        var cancelled = new java.util.concurrent.atomic.AtomicBoolean();
        fixture.provider.conflictNextWrite = ignored -> cancelled.set(true);
        assertEquals(
                ZLinkObjectCommitResult.STALE,
                fixture.repository
                        .commit(fixture.reservation, new byte[] {9}, cancelled::get)
                        .toCompletableFuture()
                        .join());
        assertEquals(1, fixture.provider.writes.size());
        var authority =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository
                                .read(fixture.reservation.authorityKey(), () -> false)
                                .toCompletableFuture()
                                .join());
        assertEquals(ZLinkPlacementAllocationState.PENDING, authority.allocation().state());
        assertTrue(authority.pendingCreation().isPresent());
    }

    @Test
    void boundedReadyRebuildsAndPublishesTerminalAtomically() {
        var fixture = new ZLinkProviderCreationTerminalTest.Fixture();
        fixture.provider.conflictNextWrite = ignored -> {};
        var terminal = fixture.terminal(ZLinkCreationTerminalState.CREATED);
        long deadlineUnixMs =
                fixture.clock
                        .instant()
                        .plus(ZLinkProviderCreationTerminalTest.ORIGINAL_DEADLINE)
                        .toEpochMilli();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                fixture.repository
                        .commit(fixture.reservation, new byte[] {9}, terminal, deadlineUnixMs)
                        .toCompletableFuture()
                        .join());
        assertEquals(2, fixture.provider.writes.size());
        var stored =
                assertInstanceOf(
                        ZLinkCreationTerminalFound.class,
                        fixture.repository
                                .readCreationTerminal(terminal.operation(), () -> false)
                                .toCompletableFuture()
                                .join());
        assertArrayEquals(terminal.terminalEnvelope(), stored.terminalEnvelope());
        var authority =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository
                                .read(fixture.reservation.authorityKey(), () -> false)
                                .toCompletableFuture()
                                .join());
        assertEquals(ZLinkPlacementAllocationState.ACTIVE, authority.allocation().state());
        assertTrue(authority.pendingCreation().isEmpty());
    }

    @Test
    void boundedTerminalFreeReadyRebuildsWithoutCreatingATerminal() {
        var fixture = new ZLinkProviderCreationTerminalTest.Fixture();
        fixture.provider.conflictNextWrite = ignored -> {};
        long deadlineUnixMs =
                fixture.clock
                        .instant()
                        .plus(ZLinkProviderCreationTerminalTest.ORIGINAL_DEADLINE)
                        .toEpochMilli();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                fixture.repository
                        .commit(fixture.reservation, new byte[] {9}, deadlineUnixMs)
                        .toCompletableFuture()
                        .join());
        assertEquals(2, fixture.provider.writes.size());
        var terminal = fixture.terminal(ZLinkCreationTerminalState.CREATED);
        assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                fixture.repository
                        .readCreationTerminal(terminal.operation(), () -> false)
                        .toCompletableFuture()
                        .join());
    }
}
