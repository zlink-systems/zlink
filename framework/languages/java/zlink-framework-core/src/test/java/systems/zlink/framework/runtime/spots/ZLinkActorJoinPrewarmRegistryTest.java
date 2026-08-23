package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

/**
 * Covers spec 15 §4.2 registry mechanics: attempt registration before the
 * Accepted reply, reuse at PREPARE, Rejected/expiry cleanup, newest-
 * attempt-wins eviction (placeholder and live), and the atomic
 * install-and-migrate transition. Ingress arrivals are always driven
 * through {@link ZLinkActorJoinPrewarmRegistry#parkOrDeliver} — the
 * production ingress entry point — never through a private queue poke;
 * the equivalent end-to-end coverage through
 * {@code ZLinkUserSpotRetireTargetEndpoint#handleActor} lives in
 * {@code ZLinkCanonicalDirectJoinHostIntegrationTest}.
 */
final class ZLinkActorJoinPrewarmRegistryTest {
    private static final String ACTOR_ID = "actor-a";
    private static final long OBJECT_GENERATION = 7L;
    private static final String ACTOR_TYPE = "TestActorType";

    @Test
    void prewarmOnAcceptRegistersTheAttemptBeforePrepareArrives() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();

        ZLinkActorJoinPrewarmRegistry.Attempt attempt = registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });

        assertTrue(registry.find(relocationId).isPresent());
        assertSame(attempt, registry.find(relocationId).orElseThrow());
        assertEquals(
            new ZLinkActorJoinPrewarmRegistry.ObjectKey(
                ACTOR_ID, OBJECT_GENERATION),
            attempt.objectKey());

        //  Production ingress parks an arrival for this object before any
        //  real stage is installed.
        var route = registry.parkOrDeliver(
            ACTOR_ID, OBJECT_GENERATION,
            new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                new byte[] {1}, null, null));
        assertEquals(ZLinkActorJoinPrewarmRegistry.IngressRoute.PARKED, route);
    }

    @Test
    void reuseOnPrepareDoesNotDoubleRegister() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        List<ZLinkActorJoinPrewarmRegistry.Attempt> evictions = new ArrayList<>();

        ZLinkActorJoinPrewarmRegistry.Attempt first = registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);
        registry.parkOrDeliver(
            ACTOR_ID, OBJECT_GENERATION,
            new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                new byte[] {1}, null, null));

        //  A retried admission round trip (or the PREPARE-time lookup)
        //  for the exact same RelocationId must observe the same
        //  instance — not a fresh, empty attempt.
        ZLinkActorJoinPrewarmRegistry.Attempt second = registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);

        assertSame(first, second);
        assertTrue(evictions.isEmpty());

        //  The parked arrival from before the reuse is still in order,
        //  migrated by the same completeMigration call below.
        List<byte[]> delivered = new ArrayList<>();
        registry.completeMigration(
            relocationId, parked -> delivered.add(parked.record()), () -> { }, () -> { });
        assertEquals(1, delivered.size());
        assertArrayRecord(new byte[] {1}, delivered.getFirst());
    }

    @Test
    void rejectCleansUpTheRegisteredAttempt() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });
        assertTrue(registry.find(relocationId).isPresent());

        //  OnActorJoin returned Rejected: the target cleans up the
        //  temporary queue and factory preparation in the same handling.
        registry.release(relocationId);

        assertFalse(registry.find(relocationId).isPresent());
        assertTrue(registry.isEmpty());
        //  A late PREPARE/Restore for the rejected identity finds nothing
        //  to reuse and cannot resurrect the discarded attempt.
        assertEquals(
            ZLinkActorJoinPrewarmRegistry.IngressRoute.NOT_FOUND,
            registry.parkOrDeliver(
                ACTOR_ID, OBJECT_GENERATION,
                new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                    new byte[] {1}, null, null)));
    }

    @Test
    void rejectFailsAnyArrivalAlreadyParked() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });
        AtomicReference<Throwable> failed = new AtomicReference<>();
        registry.parkOrDeliver(
            ACTOR_ID, OBJECT_GENERATION,
            new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                new byte[] {1}, null, failed::set));

        registry.release(relocationId);

        assertTrue(failed.get() instanceof IllegalStateException);
    }

    @Test
    void newestAttemptWinsForAPlaceholderOnlyObject() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID oldRelocationId = UUID.randomUUID();
        UUID newRelocationId = UUID.randomUUID();
        List<ZLinkActorJoinPrewarmRegistry.Attempt> evictions = new ArrayList<>();

        ZLinkActorJoinPrewarmRegistry.Attempt oldAttempt = registry.register(
            oldRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);
        AtomicReference<Throwable> failed = new AtomicReference<>();
        registry.parkOrDeliver(
            ACTOR_ID, OBJECT_GENERATION,
            new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                new byte[] {1}, null, failed::set));

        ZLinkActorJoinPrewarmRegistry.Attempt newAttempt = registry.register(
            newRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);

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

        registry.register(
            oldRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);
        //  PREPARE installs the real stage for the old identity.
        AtomicReference<Boolean> aborted = new AtomicReference<>(false);
        registry.completeMigration(
            oldRelocationId, parked -> { }, () -> { }, () -> aborted.set(true));

        //  A newer exact identity for the same object arrives before the
        //  old identity publishes: the installed-but-not-yet-published
        //  stage is aborted via its own liveAbort, not the "evicted"
        //  admission-bookkeeping callback (which never fires for an
        //  already-installed attempt).
        registry.register(
            newRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);

        assertTrue(aborted.get());
        assertTrue(evictions.isEmpty(),
            "an installed live stage is torn down through liveAbort, not "
                + "the placeholder eviction callback");
        assertFalse(registry.find(oldRelocationId).isPresent());
        assertTrue(registry.find(newRelocationId).isPresent());
    }

    @Test
    void validityWindowExpiryRemovesAnAcceptedButNeverStartedAttempt() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });
        assertTrue(registry.find(relocationId).isPresent());

        //  Accepted, but the move never started at the source
        //  (DisableRelocation/capacity/compat failure): the caller's
        //  Restore-validity timer fires this same release once the
        //  preparation validity window elapses.
        registry.release(relocationId);

        assertFalse(registry.find(relocationId).isPresent());
        assertTrue(registry.isEmpty());
    }

    @Test
    void atomicMigrationDeliversParkedArrivalsInOrderThenInstalls() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });
        registry.parkOrDeliver(
            ACTOR_ID, OBJECT_GENERATION,
            new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                new byte[] {1}, null, null));
        registry.parkOrDeliver(
            ACTOR_ID, OBJECT_GENERATION,
            new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                new byte[] {2}, null, null));

        List<String> transitions = new ArrayList<>();
        registry.completeMigration(
            relocationId,
            parked -> transitions.add("deliver:" + parked.record()[0]),
            () -> transitions.add("actorStages:insert"),
            () -> { });

        assertEquals(List.of(
            "deliver:1", "deliver:2", "actorStages:insert"), transitions,
            "the real stage is published only after every parked arrival is "
                + "migrated while the registry monitor remains held");

        //  Once installed, a further arrival for the same object is
        //  delivered straight through the installed sink — not re-parked.
        var route = registry.parkOrDeliver(
            ACTOR_ID, OBJECT_GENERATION,
            new ZLinkActorJoinPrewarmRegistry.ParkedMessage(
                new byte[] {3}, null, null));
        assertEquals(ZLinkActorJoinPrewarmRegistry.IngressRoute.DELIVERED, route,
            "once the real stage is installed, further arrivals for the "
                + "same object are delivered straight through, not parked");
    }

    @Test
    void completeMigrationRejectsAnAlreadyEvictedIdentity() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID oldRelocationId = UUID.randomUUID();
        UUID newRelocationId = UUID.randomUUID();
        registry.register(
            oldRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });
        registry.register(
            newRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });

        //  A late PREPARE for the evicted old identity must be discarded,
        //  not installed (spec 15 §4.2).
        assertThrows(IllegalStateException.class, () ->
            registry.completeMigration(
                oldRelocationId, parked -> { }, () -> { }, () -> { }));
    }

    private static void assertArrayRecord(byte[] expected, byte[] actual) {
        assertEquals(expected.length, actual.length);
        for (int index = 0; index < expected.length; index++) {
            assertEquals(expected[index], actual[index]);
        }
    }
}
