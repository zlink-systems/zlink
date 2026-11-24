package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkHostRelocationBatchTest {
    @RepeatedTest(40)
    void spotWideDoesNotStartWhileEntryActorIsPending() {
        var batch = new EntryActorBatch();
        assertEquals(0, batch.spotWideStarts.get(), "SpotWide started before Entry Actor terminal");
        batch.actorTerminal.complete(null);
        batch.hostTerminal.join();
    }

    @RepeatedTest(40)
    void entryActorRejectionPreventsSpotWideAndPreservesBlockedReason() {
        var batch = new EntryActorBatch();
        var rejected =
                new ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.RELOCATION_FAILED,
                        "target rejected PREPARE");
        batch.actorTerminal.completeExceptionally(rejected);

        assertEquals(
                0, batch.spotWideStarts.get(), "SpotWide started despite Entry Actor rejection");
        var failure = assertThrows(CompletionException.class, batch.hostTerminal::join);
        assertSame(rejected, failure.getCause());
        assertEquals(ZLinkFrameworkRelocationReason.RELOCATION_FAILED, rejected.reason());
    }

    @RepeatedTest(40)
    void entryActorSuccessStartsSpotWideAfterTerminal() {
        var batch = new EntryActorBatch();
        batch.actorTerminal.complete(null);
        batch.hostTerminal.join();

        assertEquals(1, batch.spotWideStarts.get());
        assertEquals(
                1,
                batch.spotWideStartsAfterActor.get(),
                "SpotWide must start after Actor terminal");
    }

    @Test
    void failingActorBatchWaitsForAnIndependentActorAndKeepsSpotWideOnSource() {
        var rejected = new CompletableFuture<Void>();
        var committed = new CompletableFuture<Void>();
        var spotWideStarts = new AtomicInteger();
        var actorStarts = new AtomicInteger();
        var hostTerminal =
                ZLinkUserSpotRetireRuntime.executePlan(
                                new ZLinkUserSpotRetireRuntime.RelocationPlan(
                                        List.of("spot-wide"), List.of("rejected", "committed")),
                                ignored -> CompletableFuture.completedFuture(null),
                                ignored -> CompletableFuture.completedFuture(null),
                                () -> {},
                                () -> false,
                                ignored -> {
                                    spotWideStarts.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                },
                                actorId -> {
                                    actorStarts.incrementAndGet();
                                    return actorId.equals("rejected") ? rejected : committed;
                                })
                        .toCompletableFuture();

        assertEquals(2, actorStarts.get());
        var rejection = new IllegalStateException("target rejected PREPARE");
        rejected.completeExceptionally(rejection);
        assertFalse(hostTerminal.isDone());
        committed.complete(null);

        assertEquals(0, spotWideStarts.get());
        assertSplit(hostTerminal, rejection);
    }

    @Test
    void spotWideRejectionPreservesTheEarlierActorCommitInHostResult() {
        var rejection = new IllegalStateException("target rejected PREPARE");
        var hostTerminal =
                ZLinkUserSpotRetireRuntime.executePlan(
                                new ZLinkUserSpotRetireRuntime.RelocationPlan(
                                        List.of("spot-wide"), List.of("entry-actor")),
                                ignored -> CompletableFuture.completedFuture(null),
                                ignored -> CompletableFuture.completedFuture(null),
                                () -> {},
                                () -> false,
                                ignored -> CompletableFuture.failedFuture(rejection),
                                ignored -> CompletableFuture.completedFuture(null))
                        .toCompletableFuture();

        assertSplit(hostTerminal, rejection);
    }

    private static void assertSplit(CompletableFuture<Void> hostTerminal, Throwable rejection) {
        var failure = assertThrows(CompletionException.class, hostTerminal::join);
        var split =
                assertInstanceOf(
                        ZLinkUserSpotRetireRuntime.RelocationAuthorityErrorException.class,
                        failure.getCause());
        assertSame(rejection, split.getCause());
    }

    private static final class EntryActorBatch {
        private final CompletableFuture<Void> actorTerminal = new CompletableFuture<>();
        private final AtomicInteger spotWideStarts = new AtomicInteger();
        private final AtomicInteger spotWideStartsAfterActor = new AtomicInteger();
        private final CompletableFuture<Void> hostTerminal =
                ZLinkUserSpotRetireRuntime.executePlan(
                                new ZLinkUserSpotRetireRuntime.RelocationPlan(
                                        List.of("spot-wide"), List.of("entry-actor")),
                                ignored -> CompletableFuture.completedFuture(null),
                                ignored -> CompletableFuture.completedFuture(null),
                                () -> {},
                                () -> false,
                                ignored -> {
                                    spotWideStarts.incrementAndGet();
                                    if (actorTerminal.isDone()) {
                                        spotWideStartsAfterActor.incrementAndGet();
                                    }
                                    return CompletableFuture.completedFuture(null);
                                },
                                ignored -> actorTerminal)
                        .toCompletableFuture();
    }
}
