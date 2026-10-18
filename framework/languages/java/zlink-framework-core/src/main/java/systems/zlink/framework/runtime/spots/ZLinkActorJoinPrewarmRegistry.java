package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;

/** Target-side owner of the relocation temporary queue for cross-node Actor Join admission. */
final class ZLinkActorJoinPrewarmRegistry {
    record ObjectKey(String actorId, long objectGeneration) {
        ObjectKey {
            Objects.requireNonNull(actorId, "actorId");
        }
    }

    record ParkedMessage(
            byte[] record, Consumer<List<Message>> reply, Consumer<Throwable> failure) {
        ParkedMessage {
            record = Objects.requireNonNull(record, "record").clone();
        }

        @Override
        public byte[] record() {
            return record.clone();
        }
    }

    enum IngressRoute {
        DELIVERED,
        PARKED,
        NOT_FOUND
    }

    static final class SupersededAttemptException extends IllegalStateException {
        SupersededAttemptException(UUID relocationId) {
            super(
                    "Actor Join relocation attempt was superseded before PREPARE "
                            + "installed its stage: "
                            + relocationId);
        }
    }

    static final class Attempt {
        private final UUID relocationId;
        private final ObjectKey objectKey;
        private final Queue<ParkedMessage> parked = new ArrayDeque<>();
        private Consumer<ParkedMessage> installedSink;
        private Runnable liveAbort;
        private boolean migrationInProgress;
        // The accepted canonical target attempt owns this completion.
        private CompletionStage<Void> terminal;

        private Attempt(UUID relocationId, ObjectKey objectKey) {
            this.relocationId = relocationId;
            this.objectKey = objectKey;
        }

        private boolean hasTargetOwner() {
            return terminal != null;
        }

        UUID relocationId() {
            return relocationId;
        }

        ObjectKey objectKey() {
            return objectKey;
        }
    }

    private final Consumer<UUID> released;

    ZLinkActorJoinPrewarmRegistry() {
        this(ignored -> {});
    }

    ZLinkActorJoinPrewarmRegistry(Consumer<UUID> released) {
        this.released = Objects.requireNonNull(released, "released");
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

    Attempt register(
            UUID relocationId, String actorId, long objectGeneration, Consumer<Attempt> evicted) {
        RegistrationState registration =
                inStateLane(() -> registerOnLane(relocationId, actorId, objectGeneration));
        if (registration.displaced() != null
                && !registration.displaced().attempt().hasTargetOwner()) {
            released.accept(registration.displaced().attempt().relocationId());
        }
        evictOutsideLane(registration.displaced(), evicted);
        return registration.attempt();
    }

    private RegistrationState registerOnLane(
            UUID relocationId, String actorId, long objectGeneration) {
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
        Attempt attempt = new Attempt(relocationId, key);
        byRelocation.put(relocationId, attempt);
        byObject.put(key, relocationId);
        return new RegistrationState(attempt, detachEvictionOnLane(displaced));
    }

    Optional<Attempt> find(UUID relocationId) {
        return inStateLane(() -> Optional.ofNullable(byRelocation.get(relocationId)));
    }

    IngressRoute parkOrDeliver(
            String actorId, long objectGeneration, Supplier<ParkedMessage> message) {
        DeliveryState delivery =
                inStateLane(() -> parkOrDeliverOnLane(actorId, objectGeneration, message));
        if (delivery == null) return IngressRoute.NOT_FOUND;
        if (delivery.sink() == null) return IngressRoute.PARKED;
        delivery.sink().accept(message.get());
        return IngressRoute.DELIVERED;
    }

    private DeliveryState parkOrDeliverOnLane(
            String actorId, long objectGeneration, Supplier<ParkedMessage> message) {
        UUID relocationId = byObject.get(new ObjectKey(actorId, objectGeneration));
        if (relocationId == null) return null;
        Attempt attempt = byRelocation.get(relocationId);
        if (attempt == null) return null;
        if (attempt.installedSink != null && !attempt.migrationInProgress) {
            return new DeliveryState(attempt.installedSink);
        }
        attempt.parked.add(message.get());
        return new DeliveryState(null);
    }

    void completeMigration(
            UUID relocationId,
            Consumer<ParkedMessage> deliver,
            Runnable installed,
            Runnable liveAbort) {
        MigrationState migration =
                inStateLane(() -> completeMigrationOnLane(relocationId, deliver, liveAbort));
        migration.parked().forEach(deliver);
        installed.run();
        List<ParkedMessage> arrivalsDuringInstall =
                inStateLane(
                        () ->
                                completeMigrationInstallOnLane(
                                        relocationId, migration.attempt(), deliver));
        arrivalsDuringInstall.forEach(deliver);
    }

    private MigrationState completeMigrationOnLane(
            UUID relocationId, Consumer<ParkedMessage> deliver, Runnable liveAbort) {
        Objects.requireNonNull(deliver, "deliver");
        Objects.requireNonNull(liveAbort, "liveAbort");
        Attempt attempt = byRelocation.get(relocationId);
        if (attempt == null) throw new SupersededAttemptException(relocationId);
        if (attempt.liveAbort == null) attempt.liveAbort = liveAbort;
        attempt.migrationInProgress = true;
        return new MigrationState(attempt, drainParkedOnLane(attempt));
    }

    private List<ParkedMessage> completeMigrationInstallOnLane(
            UUID relocationId, Attempt expected, Consumer<ParkedMessage> deliver) {
        if (byRelocation.get(relocationId) != expected) return List.of();
        expected.installedSink = deliver;
        expected.migrationInProgress = false;
        return drainParkedOnLane(expected);
    }

    CompletionStage<Void> begin(
            UUID relocationId, CompletionStage<Void> terminal, Runnable superseded) {
        return stateLane.runAsync(
                () -> {
                    Attempt current = byRelocation.get(relocationId);
                    if (current != null) {
                        current.terminal = terminal;
                        current.liveAbort = superseded;
                    }
                    return (Void) null;
                });
    }

    CompletionStage<Void> expire(UUID relocationId) {
        return stateLane
                .runAsync(
                        () -> {
                            Attempt attempt = byRelocation.get(relocationId);
                            return attempt == null || attempt.hasTargetOwner()
                                    ? null
                                    : releaseOnLane(relocationId);
                        })
                .thenAccept(
                        parked -> {
                            if (parked != null) finishRelease(relocationId, parked);
                        });
    }

    void detachIngress(UUID relocationId) {
        List<ParkedMessage> parked =
                inStateLane(
                        () -> {
                            Attempt attempt = byRelocation.get(relocationId);
                            if (attempt == null) return List.of();
                            byObject.remove(attempt.objectKey(), relocationId);
                            attempt.installedSink = null;
                            attempt.liveAbort = null;
                            return drainParkedOnLane(attempt);
                        });
        failParkedOutsideLane(parked);
    }

    CompletionStage<Void> releaseAsync(UUID relocationId) {
        return stateLane
                .runAsync(() -> releaseOnLane(relocationId))
                .thenAccept(parked -> finishRelease(relocationId, parked));
    }

    private void finishRelease(UUID relocationId, List<ParkedMessage> parked) {
        released.accept(relocationId);
        if (parked != null) failParkedOutsideLane(parked);
    }

    private List<ParkedMessage> releaseOnLane(UUID relocationId) {
        Attempt attempt = byRelocation.remove(relocationId);
        if (attempt == null) return null;
        byObject.remove(attempt.objectKey(), relocationId);
        return drainParkedOnLane(attempt);
    }

    boolean isEmpty() {
        return inStateLane(byRelocation::isEmpty);
    }

    private EvictionState detachEvictionOnLane(Attempt displaced) {
        return displaced == null
                ? null
                : new EvictionState(displaced, displaced.liveAbort, drainParkedOnLane(displaced));
    }

    private static List<ParkedMessage> drainParkedOnLane(Attempt attempt) {
        List<ParkedMessage> parked = List.copyOf(attempt.parked);
        attempt.parked.clear();
        return parked;
    }

    private static void evictOutsideLane(EvictionState displaced, Consumer<Attempt> evicted) {
        if (displaced == null) return;
        if (displaced.abort() != null) displaced.abort().run();
        else if (evicted != null) evicted.accept(displaced.attempt());
        failParkedOutsideLane(displaced.parked());
    }

    private static void failParkedOutsideLane(List<ParkedMessage> parked) {
        if (parked.isEmpty()) return;
        IllegalStateException aborted =
                new IllegalStateException("Actor Join relocation temporary queue was aborted");
        for (ParkedMessage message : parked) {
            if (message.failure() != null) message.failure().accept(aborted);
        }
    }

    private record RegistrationState(Attempt attempt, EvictionState displaced) {}

    private record EvictionState(Attempt attempt, Runnable abort, List<ParkedMessage> parked) {}

    private record DeliveryState(Consumer<ParkedMessage> sink) {}

    private record MigrationState(Attempt attempt, List<ParkedMessage> parked) {}
}
