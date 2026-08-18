package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import org.junit.jupiter.api.Test;

/**
 * Covers spec 15 §4.2 prewarm-on-accept behavior at the registry level:
 * temp queue registration before the Accepted reply, reuse at PREPARE,
 * Rejected cleanup, newest-attempt-wins eviction and validity-window
 * expiry (the expiry timer itself lives in the caller —
 * {@code ZLinkActorJoinCanonicalAdapter} — so this test drives it
 * directly via {@link ZLinkActorJoinPrewarmRegistry#release}).
 */
final class ZLinkActorJoinPrewarmRegistryTest {
    private static final String ACTOR_ID = "actor-a";
    private static final long OBJECT_GENERATION = 7L;
    private static final String ACTOR_TYPE = "TestActorType";

    @Test
    void prewarmOnAcceptRegistersTemporaryQueueBeforePrepareArrives() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();

        ZLinkActorJoinPrewarmRegistry.Prewarm prewarm = registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evicted -> { });

        //  The temporary queue exists — and can already park an arrival —
        //  before any PREPARE/Restore for this identity is looked up.
        assertTrue(registry.find(relocationId).isPresent());
        assertSame(prewarm, registry.find(relocationId).orElseThrow());
        prewarm.temporaryQueue().offer("early-arrival");
        assertEquals(1, prewarm.temporaryQueue().size());
        assertEquals(
            new ZLinkActorJoinPrewarmRegistry.ObjectKey(
                ACTOR_ID, OBJECT_GENERATION),
            prewarm.objectKey());
    }

    @Test
    void reuseOnPrepareDoesNotDoubleRegister() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID relocationId = UUID.randomUUID();
        List<ZLinkActorJoinPrewarmRegistry.Prewarm> evictions = new ArrayList<>();

        ZLinkActorJoinPrewarmRegistry.Prewarm first = registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);
        first.temporaryQueue().offer("m1");

        //  A retried admission round trip (or the PREPARE-time lookup)
        //  for the exact same RelocationId must observe the same
        //  instance — not a fresh, empty queue.
        ZLinkActorJoinPrewarmRegistry.Prewarm second = registry.register(
            relocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);

        assertSame(first, second);
        assertSame(first.temporaryQueue(), second.temporaryQueue());
        assertEquals(1, first.temporaryQueue().size());
        assertTrue(evictions.isEmpty());
    }

    @Test
    void rejectCleansUpTheRegisteredPrewarm() {
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
        //  to reuse and cannot resurrect the discarded prewarm.
        assertFalse(registry.find(relocationId).isPresent());
    }

    @Test
    void newestAttemptWinsForTheSameObject() {
        ZLinkActorJoinPrewarmRegistry registry = new ZLinkActorJoinPrewarmRegistry();
        UUID oldRelocationId = UUID.randomUUID();
        UUID newRelocationId = UUID.randomUUID();
        List<ZLinkActorJoinPrewarmRegistry.Prewarm> evictions = new ArrayList<>();

        ZLinkActorJoinPrewarmRegistry.Prewarm oldPrewarm = registry.register(
            oldRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);

        ZLinkActorJoinPrewarmRegistry.Prewarm newPrewarm = registry.register(
            newRelocationId, ACTOR_ID, OBJECT_GENERATION, ACTOR_TYPE,
            String.class, evictions::add);

        //  Only one relocation temporary queue exists per object: the
        //  older exact identity is aborted and cleaned up first.
        assertEquals(List.of(oldPrewarm), evictions);
        assertFalse(registry.find(oldRelocationId).isPresent());
        assertTrue(registry.find(newRelocationId).isPresent());
        assertSame(newPrewarm, registry.find(newRelocationId).orElseThrow());
    }

    @Test
    void validityWindowExpiryRemovesAnAcceptedButNeverStartedPrewarm() {
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
}
