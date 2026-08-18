package systems.zlink.framework.runtime.spots;

import java.util.ArrayDeque;
import java.util.Objects;
import java.util.Optional;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.function.Consumer;

/**
 * Target-side prewarm state for cross-node Actor Join admission (spec 15
 * §4.2). {@code OnActorJoin} registers the relocation temporary queue and
 * validates the factory for {@code ActorId + ObjectGeneration} before
 * returning {@code Accepted}, so the following PREPARE (Restore) does not
 * repeat that work. Only one prewarm exists per object; a newer exact
 * identity for the same object evicts the older one (spec 15 §4.2 "같은
 * object의 relocation temporary queue는 하나만 존재한다").
 *
 * <p>{@code targetAttemptGeneration} and the coordinator fence (spec 28
 * §3 exact identity) are not known yet at admission time — only
 * {@code RelocationId} is. This registry therefore keys prewarm state by
 * {@code RelocationId}; the caller is responsible for verifying the full
 * exact identity once it becomes available at PREPARE time.
 */
final class ZLinkActorJoinPrewarmRegistry {

    /** Identifies the object a prewarm belongs to. */
    record ObjectKey(String actorId, long objectGeneration) {
        ObjectKey {
            Objects.requireNonNull(actorId, "actorId");
        }
    }

    /**
     * Placeholder relocation temporary queue reserved at admission time.
     * Application handler execution never happens against this queue
     * (spec 15 §4.2 "temporary queue가 등록되어 있어도 Location Store CAS
     * 전에는 application handler를 실행하지 않는다") — it only parks
     * arrivals until the Restore/cutover path takes over.
     */
    static final class TemporaryQueue {
        private final Queue<Object> pending = new ArrayDeque<>();

        synchronized void offer(Object message) {
            pending.add(Objects.requireNonNull(message, "message"));
        }

        synchronized int size() {
            return pending.size();
        }
    }

    record Prewarm(
        UUID relocationId,
        ObjectKey objectKey,
        String actorType,
        Class<?> factoryType,
        TemporaryQueue temporaryQueue) {
        Prewarm {
            Objects.requireNonNull(relocationId, "relocationId");
            Objects.requireNonNull(objectKey, "objectKey");
            Objects.requireNonNull(actorType, "actorType");
            Objects.requireNonNull(factoryType, "factoryType");
            Objects.requireNonNull(temporaryQueue, "temporaryQueue");
        }
    }

    private final ConcurrentMap<UUID, Prewarm> byRelocation =
        new ConcurrentHashMap<>();
    private final ConcurrentMap<ObjectKey, UUID> byObject =
        new ConcurrentHashMap<>();

    /**
     * Registers (or reuses, for a retried admission of the same
     * RelocationId) a prewarm. If a different RelocationId already holds
     * the prewarm for the same object, it is evicted first — newest
     * attempt wins — and passed to {@code evicted} so the caller can
     * release any of its own resources tied to the old identity.
     */
    synchronized Prewarm register(
        UUID relocationId,
        String actorId,
        long objectGeneration,
        String actorType,
        Class<?> factoryType,
        Consumer<Prewarm> evicted) {
        Objects.requireNonNull(relocationId, "relocationId");
        ObjectKey key = new ObjectKey(actorId, objectGeneration);
        Prewarm existing = byRelocation.get(relocationId);
        if (existing != null) {
            //  Same exact identity retried the admission round trip:
            //  reuse rather than re-register.
            return existing;
        }
        UUID displacedId = byObject.get(key);
        if (displacedId != null && !displacedId.equals(relocationId)) {
            Prewarm displaced = byRelocation.remove(displacedId);
            byObject.remove(key, displacedId);
            if (displaced != null && evicted != null) {
                evicted.accept(displaced);
            }
        }
        Prewarm prewarm = new Prewarm(
            relocationId,
            key,
            actorType,
            factoryType,
            new TemporaryQueue());
        byRelocation.put(relocationId, prewarm);
        byObject.put(key, relocationId);
        return prewarm;
    }

    /** Looks up the prewarm for reuse at PREPARE (Restore) time. */
    Optional<Prewarm> find(UUID relocationId) {
        return Optional.ofNullable(byRelocation.get(relocationId));
    }

    /**
     * Releases the prewarm for {@code relocationId} — Rejected admission,
     * expiry of the preparation validity window, or normal consumption at
     * PREPARE/cutover.
     */
    synchronized void release(UUID relocationId) {
        Prewarm prewarm = byRelocation.remove(relocationId);
        if (prewarm != null) {
            byObject.remove(prewarm.objectKey(), relocationId);
        }
    }

    boolean isEmpty() {
        return byRelocation.isEmpty();
    }
}
