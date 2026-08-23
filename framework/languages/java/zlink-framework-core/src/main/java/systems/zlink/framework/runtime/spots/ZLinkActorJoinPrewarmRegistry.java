package systems.zlink.framework.runtime.spots;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Queue;
import java.util.UUID;
import java.util.function.Consumer;
import systems.zlink.contracts.messaging.Message;

/**
 * Target-side owner of the relocation temporary queue for cross-node Actor
 * Join admission (spec 15 §4.2). {@code OnActorJoin} registers this queue
 * and validates the factory for {@code ActorId + ObjectGeneration} before
 * returning {@code Accepted}. From that moment production ingress
 * ({@code handleActor}) must consult this registry for the object — not a
 * side structure nothing offers to — so an arrival between Accepted and
 * PREPARE parks here instead of being dropped. PREPARE installs the real
 * staged queue and atomically migrates every parked arrival into it in
 * order, in the same critical section, so ingress never observes a window
 * where the object is neither parkable nor deliverable. Only one attempt
 * exists per object; a newer exact identity evicts the older one — a
 * placeholder is simply discarded, an already-installed (not yet
 * published) real stage is aborted (spec 15 §4.2 "같은 object의 relocation
 * temporary queue는 하나만 존재한다").
 *
 * <p>All mutating and lookup operations run under this instance's
 * monitor: the admission-time registration, the ingress park/deliver
 * decision and the PREPARE-time install+drain+release transition are all
 * {@code synchronized} on {@code this}, so no two of them can interleave
 * and no caller can ever observe an in-between state.
 *
 * <p>{@code targetAttemptGeneration} and the coordinator fence (spec 28
 * §3 exact identity) are not known yet at admission time — only
 * {@code RelocationId} is. This registry therefore keys attempts by
 * {@code RelocationId}; the caller verifies the full exact identity once
 * it becomes available at PREPARE time.
 */
final class ZLinkActorJoinPrewarmRegistry {

    /** Identifies the object an attempt belongs to. */
    record ObjectKey(String actorId, long objectGeneration) {
        ObjectKey {
            Objects.requireNonNull(actorId, "actorId");
        }
    }

    /**
     * One ingress arrival, preserved verbatim (record bytes, reply and
     * failure sinks) so it can be replayed exactly as production ingress
     * would have handled it directly against the real stage.
     */
    record ParkedMessage(
        byte[] record,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        ParkedMessage {
            record = Objects.requireNonNull(record, "record").clone();
        }

        @Override public byte[] record() {
            return record.clone();
        }
    }

    /** Outcome of a production ingress lookup against this registry. */
    enum IngressRoute {
        /** Delivered straight into the real stage PREPARE installed. */
        DELIVERED,
        /** No real stage yet — parked in the placeholder queue. */
        PARKED,
        /** No attempt exists for this object at all. */
        NOT_FOUND
    }

    /** A late PREPARE reached a relocation identity displaced by a newer one. */
    static final class SupersededAttemptException extends IllegalStateException {
        SupersededAttemptException(UUID relocationId) {
            super("Actor Join relocation attempt was superseded before PREPARE "
                + "installed its stage: " + relocationId);
        }
    }

    /**
     * One in-flight relocation attempt for an object, from admission
     * through PREPARE to publish (or abort/eviction). Application handler
     * execution never happens against the parked queue (spec 15 §4.2
     * "temporary queue가 등록되어 있어도 Location Store CAS 전에는
     * application handler를 실행하지 않는다") — parked arrivals only wait
     * here until PREPARE migrates them into the real staged queue.
     */
    static final class Attempt {
        private final UUID relocationId;
        private final ObjectKey objectKey;
        private final String actorType;
        private final Class<?> factoryType;
        private final Queue<ParkedMessage> parked = new ArrayDeque<>();
        //  Non-null once PREPARE has installed the real stage: ingress
        //  delivers straight through this sink instead of parking.
        private Consumer<ParkedMessage> installedSink;
        //  Non-null in the same window: tears the installed (not yet
        //  published) real stage down if a newer exact identity evicts
        //  this attempt before publish commits.
        private Runnable liveAbort;

        private Attempt(
            UUID relocationId,
            ObjectKey objectKey,
            String actorType,
            Class<?> factoryType) {
            this.relocationId = relocationId;
            this.objectKey = objectKey;
            this.actorType = actorType;
            this.factoryType = factoryType;
        }

        UUID relocationId() {
            return relocationId;
        }

        ObjectKey objectKey() {
            return objectKey;
        }

        String actorType() {
            return actorType;
        }

        Class<?> factoryType() {
            return factoryType;
        }
    }

    private final Map<UUID, Attempt> byRelocation = new HashMap<>();
    private final Map<ObjectKey, UUID> byObject = new HashMap<>();

    /**
     * Registers (or reuses, for a retried admission of the same
     * RelocationId) an attempt. If a different RelocationId already holds
     * the attempt for the same object, it is evicted first — newest
     * attempt wins. A placeholder-only eviction is reported to
     * {@code evicted} so the caller can release its own bookkeeping tied
     * to the old identity (e.g. the admission record); an already
     * installed real stage is torn down through its own
     * {@code liveAbort} instead. Either way every arrival parked for the
     * evicted identity fails exactly once.
     */
    synchronized Attempt register(
        UUID relocationId,
        String actorId,
        long objectGeneration,
        String actorType,
        Class<?> factoryType,
        Consumer<Attempt> evicted) {
        Objects.requireNonNull(relocationId, "relocationId");
        ObjectKey key = new ObjectKey(actorId, objectGeneration);
        Attempt existing = byRelocation.get(relocationId);
        if (existing != null) {
            //  Same exact identity retried the admission round trip:
            //  reuse rather than re-register.
            return existing;
        }
        UUID displacedId = byObject.get(key);
        if (displacedId != null && !displacedId.equals(relocationId)) {
            Attempt displaced = byRelocation.remove(displacedId);
            byObject.remove(key, displacedId);
            if (displaced != null) {
                evict(displaced, evicted);
            }
        }
        Attempt attempt = new Attempt(
            relocationId, key, actorType, factoryType);
        byRelocation.put(relocationId, attempt);
        byObject.put(key, relocationId);
        return attempt;
    }

    private void evict(Attempt displaced, Consumer<Attempt> evicted) {
        Runnable abort = displaced.liveAbort;
        if (abort != null) {
            //  Never block the registry monitor on the teardown: the
            //  callback fires an async discard and must not join it.
            abort.run();
        } else if (evicted != null) {
            evicted.accept(displaced);
        }
        failParked(displaced);
    }

    private static void failParked(Attempt attempt) {
        IllegalStateException aborted = new IllegalStateException(
            "Actor Join relocation temporary queue was aborted");
        ParkedMessage message;
        while ((message = attempt.parked.poll()) != null) {
            if (message.failure() != null) {
                message.failure().accept(aborted);
            }
        }
    }

    /** Looks up the attempt for reuse at PREPARE (Restore) time. */
    synchronized Optional<Attempt> find(UUID relocationId) {
        return Optional.ofNullable(byRelocation.get(relocationId));
    }

    /**
     * Production ingress lookup (spec 15 §4.2). Delivers straight into
     * the real stage if PREPARE already installed one, parks into the
     * placeholder if only the admission-time attempt exists, or reports
     * not-found. Runs under this registry's monitor together with
     * {@link #completeMigration}, so no caller can ever observe an
     * in-between state where the object is neither parkable nor
     * deliverable.
     */
    synchronized IngressRoute parkOrDeliver(
        String actorId, long objectGeneration, ParkedMessage message) {
        UUID relocationId = byObject.get(
            new ObjectKey(actorId, objectGeneration));
        if (relocationId == null) {
            return IngressRoute.NOT_FOUND;
        }
        Attempt attempt = byRelocation.get(relocationId);
        if (attempt == null) {
            return IngressRoute.NOT_FOUND;
        }
        if (attempt.installedSink != null) {
            attempt.installedSink.accept(message);
            return IngressRoute.DELIVERED;
        }
        attempt.parked.add(message);
        return IngressRoute.PARKED;
    }

    /**
     * Variant used by the production target stage. {@code installed} publishes
     * the real stage only after every parked arrival has moved, while this
     * monitor is still held. A newer attempt therefore cannot run liveAbort
     * between a failed remove and the later actorStages insertion.
     */
    synchronized void completeMigration(
        UUID relocationId,
        Consumer<ParkedMessage> deliver,
        Runnable installed,
        Runnable liveAbort) {
        Objects.requireNonNull(deliver, "deliver");
        Objects.requireNonNull(installed, "installed");
        Objects.requireNonNull(liveAbort, "liveAbort");
        Attempt attempt = byRelocation.get(relocationId);
        if (attempt == null) {
            throw new SupersededAttemptException(relocationId);
        }
        attempt.installedSink = deliver;
        attempt.liveAbort = liveAbort;
        ParkedMessage message;
        while ((message = attempt.parked.poll()) != null) {
            deliver.accept(message);
        }
        installed.run();
    }

    /**
     * Releases the attempt for {@code relocationId} — Rejected admission,
     * expiry of the preparation validity window, explicit target abort,
     * or normal consumption once publish commits. Any arrival still
     * parked at release time fails exactly once, mirroring the discard
     * rules the real staged queue applies to a dead identity.
     */
    synchronized void release(UUID relocationId) {
        Attempt attempt = byRelocation.remove(relocationId);
        if (attempt != null) {
            byObject.remove(attempt.objectKey(), relocationId);
            failParked(attempt);
        }
    }

    synchronized boolean isEmpty() {
        return byRelocation.isEmpty();
    }
}
