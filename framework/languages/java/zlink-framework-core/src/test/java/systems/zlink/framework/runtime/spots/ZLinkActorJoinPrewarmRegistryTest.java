package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Covers spec 15 §4.2 registry mechanics: attempt registration before the Accepted reply, reuse at
 * PREPARE, Rejected/expiry cleanup, newest- attempt-wins eviction (placeholder and live), and the
 * atomic install-and-migrate transition. Ingress arrivals are always driven through {@link
 * ZLinkActorJoinPrewarmRegistry#parkOrDeliver} — the production ingress entry point — never through
 * a private queue poke; the equivalent end-to-end coverage through {@code
 * ZLinkUserSpotRetireTargetEndpoint#handleActor} lives in {@code
 * ZLinkCanonicalDirectJoinHostIntegrationTest}.
 */
final class ZLinkActorJoinPrewarmRegistryTest {
    private static final String ACTOR_ID = "actor-a";
    private static final long OBJECT_GENERATION = 7L;

    @Test
    void journalFactoryRunsOnlyAfterTheIngressOwnerAccepts() {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        var evaluations = new java.util.concurrent.atomic.AtomicInteger();
        java.util.function.Supplier<ZLinkActorJoinPrewarmRegistry.ParkedMessage> factory =
                () -> {
                    evaluations.incrementAndGet();
                    return new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                            new byte[] {1}, null, null);
                };
        assertEquals(
                ZLinkActorJoinPrewarmRegistry.IngressRoute.NOT_FOUND,
                registry.parkOrDeliver(ACTOR_ID, OBJECT_GENERATION, factory));
        assertEquals(0, evaluations.get());
        var relocation = UUID.randomUUID();
        registry.register(relocation, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        assertEquals(
                ZLinkActorJoinPrewarmRegistry.IngressRoute.PARKED,
                registry.parkOrDeliver(ACTOR_ID, OBJECT_GENERATION, factory));
        assertEquals(1, evaluations.get());
        var delivered = new java.util.concurrent.atomic.AtomicInteger();
        registry.completeMigration(
                relocation, ignored -> delivered.incrementAndGet(), () -> {}, () -> {});
        assertEquals(1, delivered.get());
        assertEquals(
                ZLinkActorJoinPrewarmRegistry.IngressRoute.DELIVERED,
                registry.parkOrDeliver(ACTOR_ID, OBJECT_GENERATION, factory));
        assertEquals(2, evaluations.get());
        assertEquals(2, delivered.get());
    }

    @Test
    void supersessionUsesTheAcceptedTargetOwnerAfterStageInstallation() {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        UUID old = UUID.randomUUID();
        var ownerAbort = new java.util.concurrent.atomic.AtomicInteger();
        var stageAbort = new java.util.concurrent.atomic.AtomicInteger();
        registry.register(old, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        var terminal = new java.util.concurrent.CompletableFuture<Void>();
        registry.begin(old, terminal, ownerAbort::incrementAndGet).toCompletableFuture().join();
        registry.completeMigration(old, ignored -> {}, () -> {}, stageAbort::incrementAndGet);
        registry.register(UUID.randomUUID(), ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        assertEquals(1, ownerAbort.get());
        assertEquals(0, stageAbort.get());
        assertFalse(terminal.isDone(), "the accepted restore still owns terminal completion");
        registry.releaseAsync(old).toCompletableFuture().join();
        terminal.complete(null);
    }

    @Test
    void supersededRestoreKeepsItsClaimedAdmissionUntilTheTargetTerminal() {
        var resources = new java.util.concurrent.ConcurrentHashMap<UUID, UUID>();
        var registry = new ZLinkActorJoinPrewarmRegistry(resources::remove);
        UUID old = UUID.randomUUID();
        UUID replacement = UUID.randomUUID();
        registry.register(old, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        var terminal = new java.util.concurrent.CompletableFuture<Void>();
        registry.begin(old, terminal, () -> {}).toCompletableFuture().join();
        resources.put(old, old);
        registry.register(replacement, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        assertTrue(resources.containsKey(old), "the accepted target owns its bound admission");
        // Recovery may finish its claim after the old queue was evicted.
        resources.put(old, old);
        registry.releaseAsync(old).toCompletableFuture().join();
        terminal.complete(null);
        assertFalse(resources.containsKey(old), "terminal cleanup includes the late claim");
        assertTrue(registry.find(replacement).isPresent());
    }

    @Test
    void preparationExpiryReleasesUnstartedResources() {
        List<UUID> released = new ArrayList<>();
        var registry = new ZLinkActorJoinPrewarmRegistry(released::add);
        UUID id = UUID.randomUUID();
        registry.register(id, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        registry.expire(id).toCompletableFuture().join();
        assertTrue(registry.isEmpty());
        assertEquals(List.of(id), released);
    }

    @Test
    void acceptedPrepareOwnsResourcesUntilItsTerminalEvenBeforeRestore() {
        List<UUID> released = new ArrayList<>();
        var registry = new ZLinkActorJoinPrewarmRegistry(released::add);
        UUID id = UUID.randomUUID();
        registry.register(id, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        var terminal = new java.util.concurrent.CompletableFuture<Void>();
        registry.begin(id, terminal, () -> {}).toCompletableFuture().join();
        registry.expire(id).toCompletableFuture().join();
        assertTrue(registry.find(id).isPresent());
        assertTrue(released.isEmpty());
        registry.releaseAsync(id).toCompletableFuture().join();
        terminal.complete(null);
        assertTrue(registry.isEmpty());
        assertEquals(List.of(id), released);
    }

    @Test
    void failedRestoreTerminalReleasesResourcesOnce() {
        List<UUID> released = new ArrayList<>();
        var registry = new ZLinkActorJoinPrewarmRegistry(released::add);
        UUID id = UUID.randomUUID();
        registry.register(id, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        var terminal = new java.util.concurrent.CompletableFuture<Void>();
        registry.begin(id, terminal, () -> {}).toCompletableFuture().join();
        registry.releaseAsync(id).toCompletableFuture().join();
        terminal.completeExceptionally(new IllegalStateException("restore failed"));
        registry.expire(id).toCompletableFuture().join();
        assertTrue(registry.isEmpty());
        assertEquals(List.of(id), released);
    }

    @Test
    void newerIdentityReplacesActivePrepareAndOldTerminalPreservesReplacement() {
        List<UUID> released = new ArrayList<>();
        var registry = new ZLinkActorJoinPrewarmRegistry(released::add);
        UUID old = UUID.randomUUID();
        UUID replacement = UUID.randomUUID();
        registry.register(old, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        var terminal = new java.util.concurrent.CompletableFuture<Void>();
        registry.begin(old, terminal, () -> {}).toCompletableFuture().join();
        registry.register(replacement, ACTOR_ID, OBJECT_GENERATION, ignored -> {});
        assertTrue(registry.find(old).isEmpty());
        registry.releaseAsync(old).toCompletableFuture().join();
        terminal.complete(null);
        assertTrue(registry.find(replacement).isPresent());
        assertEquals(List.of(old), released);
    }

    @Test
    void prewarmOnAcceptRegistersTheAttemptBeforePrepareArrives() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();

        ZLinkActorJoinPrewarmRegistry.Attempt attempt =
                registry.register(relocationId, ACTOR_ID, OBJECT_GENERATION, evicted -> {});

        assertTrue(registry.find(relocationId).isPresent());
        assertSame(attempt, registry.find(relocationId).orElseThrow());
        assertEquals(
                new ZLinkActorJoinPrewarmRegistry.ObjectKey(ACTOR_ID, OBJECT_GENERATION),
                attempt.objectKey());

        //  Production ingress parks an arrival for this object before any
        //  real stage is installed.
        var route =
                registry.parkOrDeliver(
                        ACTOR_ID,
                        OBJECT_GENERATION,
                        () ->
                                new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                                        new byte[] {1}, null, null));
        assertEquals(ZLinkActorJoinPrewarmRegistry.IngressRoute.PARKED, route);
    }

    @Test
    void reuseOnPrepareDoesNotDoubleRegister() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        List<ZLinkActorJoinPrewarmRegistry.Attempt> evictions = new ArrayList<>();

        ZLinkActorJoinPrewarmRegistry.Attempt first =
                registry.register(relocationId, ACTOR_ID, OBJECT_GENERATION, evictions::add);
        registry.parkOrDeliver(
                ACTOR_ID,
                OBJECT_GENERATION,
                () -> new ZLinkActorJoinPrewarmRegistry.ParkedMessage(new byte[] {1}, null, null));

        //  A retried admission round trip (or the PREPARE-time lookup)
        //  for the exact same RelocationId must observe the same
        //  instance — not a fresh, empty attempt.
        ZLinkActorJoinPrewarmRegistry.Attempt second =
                registry.register(relocationId, ACTOR_ID, OBJECT_GENERATION, evictions::add);

        assertSame(first, second);
        assertTrue(evictions.isEmpty());

        //  The parked arrival from before the reuse is still in order,
        //  migrated by the same completeMigration call below.
        List<byte[]> delivered = new ArrayList<>();
        registry.completeMigration(
                relocationId, parked -> delivered.add(parked.record()), () -> {}, () -> {});
        assertEquals(1, delivered.size());
        assertArrayRecord(new byte[] {1}, delivered.getFirst());
    }

    @Test
    void rejectCleansUpTheRegisteredAttempt() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(relocationId, ACTOR_ID, OBJECT_GENERATION, evicted -> {});
        assertTrue(registry.find(relocationId).isPresent());

        //  OnActorJoin returned Rejected: the target cleans up the
        //  temporary queue and factory preparation in the same handling.
        registry.releaseAsync(relocationId).toCompletableFuture().join();

        assertFalse(registry.find(relocationId).isPresent());
        assertTrue(registry.isEmpty());
        //  A late PREPARE/Restore for the rejected identity finds nothing
        //  to reuse and cannot resurrect the discarded attempt.
        assertEquals(
                ZLinkActorJoinPrewarmRegistry.IngressRoute.NOT_FOUND,
                registry.parkOrDeliver(
                        ACTOR_ID,
                        OBJECT_GENERATION,
                        () ->
                                new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                                        new byte[] {1}, null, null)));
    }

    @Test
    void rejectFailsAnyArrivalAlreadyParked() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(relocationId, ACTOR_ID, OBJECT_GENERATION, evicted -> {});
        AtomicReference<Throwable> failed = new AtomicReference<>();
        registry.parkOrDeliver(
                ACTOR_ID,
                OBJECT_GENERATION,
                () ->
                        new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                                new byte[] {1}, null, failed::set));

        registry.releaseAsync(relocationId).toCompletableFuture().join();

        assertTrue(failed.get() instanceof IllegalStateException);
    }

    @Test
    void newestAttemptWinsForAPlaceholderOnlyObject() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID oldRelocationId = UUID.randomUUID();
        UUID newRelocationId = UUID.randomUUID();
        List<ZLinkActorJoinPrewarmRegistry.Attempt> evictions = new ArrayList<>();

        ZLinkActorJoinPrewarmRegistry.Attempt oldAttempt =
                registry.register(oldRelocationId, ACTOR_ID, OBJECT_GENERATION, evictions::add);
        AtomicReference<Throwable> failed = new AtomicReference<>();
        registry.parkOrDeliver(
                ACTOR_ID,
                OBJECT_GENERATION,
                () ->
                        new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                                new byte[] {1}, null, failed::set));

        ZLinkActorJoinPrewarmRegistry.Attempt newAttempt =
                registry.register(newRelocationId, ACTOR_ID, OBJECT_GENERATION, evictions::add);

        //  Only one relocation temporary queue exists per object: the
        //  older exact identity is aborted, its parked arrival is failed
        //  exactly once, and the eviction is reported so the caller can
        //  release its own admission bookkeeping for the old identity.
        assertEquals(List.of(oldAttempt), evictions);
        assertTrue(failed.get() instanceof IllegalStateException);
        assertFalse(registry.find(oldRelocationId).isPresent());
        assertTrue(registry.find(newRelocationId).isPresent());
        assertSame(newAttempt, registry.find(newRelocationId).orElseThrow());
    }

    @Test
    void newestAttemptAbortsAnAlreadyInstalledLiveStage() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID oldRelocationId = UUID.randomUUID();
        UUID newRelocationId = UUID.randomUUID();
        List<ZLinkActorJoinPrewarmRegistry.Attempt> evictions = new ArrayList<>();

        registry.register(oldRelocationId, ACTOR_ID, OBJECT_GENERATION, evictions::add);
        //  PREPARE installs the real stage for the old identity.
        AtomicReference<Boolean> aborted = new AtomicReference<>(false);
        registry.completeMigration(
                oldRelocationId, parked -> {}, () -> {}, () -> aborted.set(true));

        //  A newer exact identity for the same object arrives before the
        //  old identity publishes: the installed-but-not-yet-published
        //  stage is aborted via its own liveAbort, not the "evicted"
        //  admission-bookkeeping callback (which never fires for an
        //  already-installed attempt).
        registry.register(newRelocationId, ACTOR_ID, OBJECT_GENERATION, evictions::add);

        assertTrue(aborted.get());
        assertTrue(
                evictions.isEmpty(),
                "an installed live stage is torn down through liveAbort, not "
                        + "the placeholder eviction callback");
        assertFalse(registry.find(oldRelocationId).isPresent());
        assertTrue(registry.find(newRelocationId).isPresent());
    }

    @Test
    void validityWindowExpiryRemovesAnAcceptedButNeverStartedAttempt() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(relocationId, ACTOR_ID, OBJECT_GENERATION, evicted -> {});
        assertTrue(registry.find(relocationId).isPresent());

        //  Accepted, but the move never started at the source
        //  (DisableRelocation/capacity/compat failure): the caller's
        //  Restore-validity timer fires this same release once the
        //  preparation validity window elapses.
        registry.releaseAsync(relocationId).toCompletableFuture().join();

        assertFalse(registry.find(relocationId).isPresent());
        assertTrue(registry.isEmpty());
    }

    @Test
    void atomicMigrationDeliversParkedArrivalsInOrderThenInstalls() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(relocationId, ACTOR_ID, OBJECT_GENERATION, evicted -> {});
        registry.parkOrDeliver(
                ACTOR_ID,
                OBJECT_GENERATION,
                () -> new ZLinkActorJoinPrewarmRegistry.ParkedMessage(new byte[] {1}, null, null));
        registry.parkOrDeliver(
                ACTOR_ID,
                OBJECT_GENERATION,
                () -> new ZLinkActorJoinPrewarmRegistry.ParkedMessage(new byte[] {2}, null, null));

        List<String> transitions = new ArrayList<>();
        registry.completeMigration(
                relocationId,
                parked -> transitions.add("deliver:" + parked.record()[0]),
                () -> transitions.add("actorStages:insert"),
                () -> {});

        assertEquals(
                List.of("deliver:1", "deliver:2", "actorStages:insert"),
                transitions,
                "the real stage is published only after every parked arrival is "
                        + "migrated while the registry monitor remains held");

        //  Once installed, a further arrival for the same object is
        //  delivered straight through the installed sink — not re-parked.
        var route =
                registry.parkOrDeliver(
                        ACTOR_ID,
                        OBJECT_GENERATION,
                        () ->
                                new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                                        new byte[] {3}, null, null));
        assertEquals(
                ZLinkActorJoinPrewarmRegistry.IngressRoute.DELIVERED,
                route,
                "once the real stage is installed, further arrivals for the "
                        + "same object are delivered straight through, not parked");
    }

    @Test
    void completeMigrationRejectsAnAlreadyEvictedIdentity() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID oldRelocationId = UUID.randomUUID();
        UUID newRelocationId = UUID.randomUUID();
        registry.register(oldRelocationId, ACTOR_ID, OBJECT_GENERATION, evicted -> {});
        registry.register(newRelocationId, ACTOR_ID, OBJECT_GENERATION, evicted -> {});

        //  A late PREPARE for the evicted old identity must be discarded,
        //  not installed (spec 15 §4.2).
        assertThrows(
                IllegalStateException.class,
                () ->
                        registry.completeMigration(
                                oldRelocationId, parked -> {}, () -> {}, () -> {}));
    }

    private static void assertArrayRecord(byte[] expected, byte[] actual) {
        assertEquals(expected.length, actual.length);
        for (int index = 0; index < expected.length; index++) {
            assertEquals(expected[index], actual[index]);
        }
    }
}
