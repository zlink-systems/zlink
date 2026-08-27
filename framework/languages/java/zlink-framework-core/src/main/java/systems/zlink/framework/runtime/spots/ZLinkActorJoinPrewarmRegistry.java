package systems.zlink.framework.runtime.spots;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.CompletionException;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/** Target-side owner of the relocation temporary queue for cross-node Actor Join admission. */
final class ZLinkActorJoinPrewarmRegistry {
    record ObjectKey(String actorId, long objectGeneration) {
        ObjectKey { Objects.requireNonNull(actorId, "actorId"); }
    }

    record ParkedMessage(byte[] record, Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        ParkedMessage { record = Objects.requireNonNull(record, "record").clone(); }
        @Override public byte[] record() { return record.clone(); }
    }

    enum IngressRoute { DELIVERED, PARKED, NOT_FOUND }

    static final class SupersededAttemptException extends IllegalStateException {
        SupersededAttemptException(UUID relocationId) {
            super("Actor Join relocation attempt was superseded before PREPARE "
                + "installed its stage: " + relocationId);
        }
    }

    static final class Attempt {
        private final UUID relocationId;
        private final ObjectKey objectKey;
        private final String actorType;
        private final Class<?> factoryType;
        private final Queue<ParkedMessage> parked = new ArrayDeque<>();
        private Consumer<ParkedMessage> installedSink;
        private Runnable liveAbort;
        private boolean migrationInProgress;

        private Attempt(UUID relocationId, ObjectKey objectKey, String actorType,
            Class<?> factoryType) {
            this.relocationId = relocationId;
            this.objectKey = objectKey;
            this.actorType = actorType;
            this.factoryType = factoryType;
        }

        UUID relocationId() { return relocationId; }
        ObjectKey objectKey() { return objectKey; }
        String actorType() { return actorType; }
        Class<?> factoryType() { return factoryType; }
    }

    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<UUID, Attempt> byRelocation = new HashMap<>();
    private final Map<ObjectKey, UUID> byObject = new HashMap<>();

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) throw runtimeFailure;
            if (cause instanceof Error error) throw error;
            throw failure;
        }
    }

    Attempt register(UUID relocationId, String actorId, long objectGeneration,
        String actorType, Class<?> factoryType, Consumer<Attempt> evicted) {
        RegistrationState registration = inStateLane(() -> registerOnLane(
            relocationId, actorId, objectGeneration, actorType, factoryType));
        evictOutsideLane(registration.displaced(), evicted);
        return registration.attempt();
    }

    private RegistrationState registerOnLane(UUID relocationId, String actorId,
        long objectGeneration, String actorType, Class<?> factoryType) {
        Objects.requireNonNull(relocationId, "relocationId");
        ObjectKey key = new ObjectKey(actorId, objectGeneration);
        Attempt existing = byRelocation.get(relocationId);
        if (existing != null) return new RegistrationState(existing, null);
        Attempt displaced = null;
        UUID displacedId = byObject.get(key);
        if (displacedId != null && !displacedId.equals(relocationId)) {
            displaced = byRelocation.remove(displacedId);
            byObject.remove(key, displacedId);
        }
        Attempt attempt = new Attempt(relocationId, key, actorType, factoryType);
        byRelocation.put(relocationId, attempt);
        byObject.put(key, relocationId);
        return new RegistrationState(attempt, detachEvictionOnLane(displaced));
    }

    Optional<Attempt> find(UUID relocationId) {
        return inStateLane(() -> Optional.ofNullable(byRelocation.get(relocationId)));
    }

    IngressRoute parkOrDeliver(String actorId, long objectGeneration,
        ParkedMessage message) {
        DeliveryState delivery = inStateLane(() -> parkOrDeliverOnLane(
            actorId, objectGeneration, message));
        if (delivery == null) return IngressRoute.NOT_FOUND;
        if (delivery.sink() == null) return IngressRoute.PARKED;
        delivery.sink().accept(message);
        return IngressRoute.DELIVERED;
    }

    private DeliveryState parkOrDeliverOnLane(String actorId, long objectGeneration,
        ParkedMessage message) {
        UUID relocationId = byObject.get(new ObjectKey(actorId, objectGeneration));
        if (relocationId == null) return null;
        Attempt attempt = byRelocation.get(relocationId);
        if (attempt == null) return null;
        if (attempt.installedSink != null && !attempt.migrationInProgress) {
            return new DeliveryState(attempt.installedSink);
        }
        attempt.parked.add(message);
        return new DeliveryState(null);
    }

    void completeMigration(UUID relocationId, Consumer<ParkedMessage> deliver,
        Runnable installed, Runnable liveAbort) {
        MigrationState migration = inStateLane(() -> completeMigrationOnLane(
            relocationId, deliver, liveAbort));
        migration.parked().forEach(deliver);
        installed.run();
        List<ParkedMessage> arrivalsDuringInstall = inStateLane(
            () -> completeMigrationInstallOnLane(
                relocationId, migration.attempt(), deliver));
        arrivalsDuringInstall.forEach(deliver);
    }

    private MigrationState completeMigrationOnLane(UUID relocationId,
        Consumer<ParkedMessage> deliver, Runnable liveAbort) {
        Objects.requireNonNull(deliver, "deliver");
        Objects.requireNonNull(liveAbort, "liveAbort");
        Attempt attempt = byRelocation.get(relocationId);
        if (attempt == null) throw new SupersededAttemptException(relocationId);
        attempt.liveAbort = liveAbort;
        attempt.migrationInProgress = true;
        return new MigrationState(attempt, drainParkedOnLane(attempt));
    }

    private List<ParkedMessage> completeMigrationInstallOnLane(UUID relocationId,
        Attempt expected, Consumer<ParkedMessage> deliver) {
        if (byRelocation.get(relocationId) != expected) return List.of();
        expected.installedSink = deliver;
        expected.migrationInProgress = false;
        return drainParkedOnLane(expected);
    }

    void release(UUID relocationId) {
        failParkedOutsideLane(inStateLane(() -> releaseOnLane(relocationId)));
    }

    private List<ParkedMessage> releaseOnLane(UUID relocationId) {
        Attempt attempt = byRelocation.remove(relocationId);
        if (attempt == null) return List.of();
        byObject.remove(attempt.objectKey(), relocationId);
        return drainParkedOnLane(attempt);
    }

    boolean isEmpty() { return inStateLane(byRelocation::isEmpty); }

    private EvictionState detachEvictionOnLane(Attempt displaced) {
        return displaced == null ? null : new EvictionState(displaced,
            displaced.liveAbort, drainParkedOnLane(displaced));
    }

    private static List<ParkedMessage> drainParkedOnLane(Attempt attempt) {
        List<ParkedMessage> parked = List.copyOf(attempt.parked);
        attempt.parked.clear();
        return parked;
    }

    private static void evictOutsideLane(EvictionState displaced,
        Consumer<Attempt> evicted) {
        if (displaced == null) return;
        if (displaced.abort() != null) displaced.abort().run();
        else if (evicted != null) evicted.accept(displaced.attempt());
        failParkedOutsideLane(displaced.parked());
    }

    private static void failParkedOutsideLane(List<ParkedMessage> parked) {
        if (parked.isEmpty()) return;
        IllegalStateException aborted = new IllegalStateException(
            "Actor Join relocation temporary queue was aborted");
        for (ParkedMessage message : parked) {
            if (message.failure() != null) message.failure().accept(aborted);
        }
    }

    private record RegistrationState(Attempt attempt, EvictionState displaced) { }
    private record EvictionState(Attempt attempt, Runnable abort,
        List<ParkedMessage> parked) { }
    private record DeliveryState(Consumer<ParkedMessage> sink) { }
    private record MigrationState(Attempt attempt, List<ParkedMessage> parked) { }
}
