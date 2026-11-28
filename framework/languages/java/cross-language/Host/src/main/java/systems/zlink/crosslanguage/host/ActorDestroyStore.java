package systems.zlink.crosslanguage.host;

import static systems.zlink.crosslanguage.host.CrossLanguageOutageStore.actorPayload;
import static systems.zlink.crosslanguage.host.CrossLanguageOutageStore.json;
import static systems.zlink.crosslanguage.host.CrossLanguageOutageStore.key;
import static systems.zlink.crosslanguage.host.CrossLanguageOutageStore.text;

import com.fasterxml.jackson.databind.JsonNode;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;

import java.time.Instant;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;

/** Observes canonical Store records and gates only the selected provider submission. */
final class ActorDestroyStore implements ZLinkLocationStore, AutoCloseable {
    private static final ZLinkStoreCancellation ACTIVE = () -> false;
    private final ZLinkRedisLocationStore redis;
    private final ZLinkStoreKey actorKey;
    private final boolean owner;
    private final String scenario;
    private final ActorDestroyEvents events;
    private final SubmissionGate delete = new SubmissionGate();
    private final SubmissionGate closing = new SubmissionGate();
    private final AtomicBoolean createObservedClosing = new AtomicBoolean();
    private final AtomicBoolean validClosingObserved = new AtomicBoolean();
    private final Set<CompletionStage<?>> leaseWrites = new HashSet<>();
    private volatile Authority original;
    private volatile Instant originalLeaseExpiry;
    private volatile boolean creating;
    private volatile boolean ownerClosingCommitted;
    private ZLinkStoreKey blockedLease;

    ActorDestroyStore(ZLinkRedisLocationStore redis, HostArgs args, EventSink sink) {
        this.redis = redis;
        actorKey = key("authority", "actor", args.require("actor-id"));
        owner = args.mode().equals("actor-destroy-owner");
        scenario = args.require("actor-destroy-scenario");
        events = new ActorDestroyEvents(sink, args.require("actor-destroy-run-id"));
    }

    ActorDestroyEvents events() {
        return events;
    }

    void armDelete() {
        delete.arm();
    }

    void resumeDelete() {
        delete.release(() -> events.emit("ownerDeleteResumed"), null);
    }

    void armClosing() {
        closing.arm();
    }

    void failClosing() {
        closing.release(() -> events.emit("closingCasFailed"), new InjectedClosingCasFailure());
    }

    void cancelHolds() {
        delete.cancel();
        closing.cancel();
    }

    void startingCreation() {
        creating = true;
    }

    void factoryStarted() {
        if (!owner && creating) events.emit("newFactoryStarted");
    }

    Authority authority() {
        return await(redis.read(actorKey, ACTIVE)) instanceof ZLinkStoreReadFound found
                ? decode(found.value())
                : null;
    }

    void rememberOriginal(Authority authority) {
        if (original == null) {
            original = authority;
            lease();
        }
    }

    boolean placementPublished(String mesh, RoutingId rid, int weight) {
        var descriptorKey = key("mesh-node", mesh, rid.toHex());
        return await(redis.read(descriptorKey, ACTIVE)) instanceof ZLinkStoreReadFound found
                && json(found.value().bytes()).path("descriptor").path("placementWeight").asInt(-1)
                        == weight;
    }

    String lease() {
        Authority authority = requireOriginal();
        return leaseState(authority, await(redis.read(authority.leaseKey(), ACTIVE)));
    }

    private String leaseState(Authority authority, ZLinkStoreReadResult read) {
        if (read instanceof ZLinkStoreReadFound found) {
            if (!authority.matchesLease(found.value())) return "replaced";
            Instant expiry = found.value().expiresAt();
            originalLeaseExpiry = expiry;
            return expiry.isAfter(found.value().storeNow()) ? "valid" : "expired";
        }
        var missing = (ZLinkStoreReadMissing) read;
        Instant expiry = originalLeaseExpiry;
        return expiry != null && !missing.storeNow().isBefore(expiry) ? "expired" : "missing";
    }

    void endLease() {
        Authority authority = requireOriginal();
        ZLinkStoreKey leaseKey = authority.leaseKey();
        CompletableFuture<?>[] pending;
        synchronized (this) {
            blockedLease = leaseKey;
            pending =
                    leaseWrites.stream()
                            .map(
                                    stage ->
                                            stage.handle((result, failure) -> null)
                                                    .toCompletableFuture())
                            .toArray(CompletableFuture[]::new);
        }
        CompletableFuture.allOf(pending).join();
        var current = await(redis.read(leaseKey, ACTIVE));
        if (!(current instanceof ZLinkStoreReadFound found)
                || !authority.matchesLease(found.value())
                || !found.value().expiresAt().isAfter(found.value().storeNow())) {
            throw new IllegalStateException("original owner lease is no longer valid");
        }
        var request =
                new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(leaseKey, found.value().version())),
                        List.of(new ZLinkStoreDelete(leaseKey)));
        if (!(await(redis.write(request, ACTIVE)) instanceof ZLinkStoreWriteApplied applied)) {
            throw new IllegalStateException("conditional owner lease termination failed");
        }
        events.emitAt(applied.storeNow(), "ownerLeaseEnded");
    }

    private Authority requireOriginal() {
        return java.util.Objects.requireNonNull(original, "original authority was not observed");
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
        return redis.read(key, cancellation)
                .thenCompose(
                        result -> {
                            Authority previous = original;
                            if (owner
                                    || previous == null
                                    || !actorKey.equals(key)
                                    || !(result instanceof ZLinkStoreReadFound found)) {
                                return CompletableFuture.completedFuture(result);
                            }
                            Authority authority = decode(found.value());
                            if (!authority.generation().equals(previous.generation())
                                    || !authority.closing()) {
                                return CompletableFuture.completedFuture(result);
                            }
                            if (creating && createObservedClosing.compareAndSet(false, true)) {
                                events.emit("createObservedClosing");
                            }
                            if (!scenario.equals("f") || validClosingObserved.get()) {
                                return CompletableFuture.completedFuture(result);
                            }
                            return redis.read(previous.leaseKey(), cancellation)
                                    .thenApply(
                                            read -> {
                                                if (leaseState(previous, read).equals("valid")
                                                        && validClosingObserved.compareAndSet(
                                                                false, true)) {
                                                    events.emit(
                                                            "callerObservedClosingWithValidLease");
                                                }
                                                return result;
                                            });
                        });
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
        return redis.scan(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        for (var mutation : request.mutations()) {
            if (!mutation.key().equals(actorKey)) continue;
            switch (mutation) {
                case ZLinkStoreDelete ignored -> {
                    if (owner && ownerClosingCommitted) {
                        return delete.beforeSubmission(() -> events.emit("ownerDeleteHeld"))
                                .thenCompose(
                                        unused ->
                                                commit(
                                                        request,
                                                        cancellation,
                                                        "ownerDeleteCommitted"));
                    }
                    var previous = original;
                    if (!owner && previous != null && conditioned(request, previous.leaseKey())) {
                        return release(request, cancellation, previous);
                    }
                }
                case ZLinkStorePut put -> {
                    var payload = actorPayload(json(put.bytes()));
                    if (owner && payload.state() == 2 && payload.operation() == 3) {
                        return closing.beforeSubmission(() -> events.emit("closingCasHeld"))
                                .thenCompose(
                                        unused ->
                                                commit(request, cancellation, "closingCommitted"));
                    }
                    if (!owner && creating && payload.steadyReady()) {
                        return commit(request, cancellation, "newReadyCommitted");
                    }
                }
                default -> {}
            }
        }
        if (owner
                && request.mutations().stream()
                        .anyMatch(mutation -> mutation.key().value().startsWith("owner-lease\0"))) {
            return writeLease(request, cancellation);
        }
        return writeBackend(request, cancellation);
    }

    private CompletionStage<ZLinkStoreWriteResult> writeBackend(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        var result = redis.write(request, cancellation);
        if (request.mutations().stream().noneMatch(mutation -> mutation.key().equals(actorKey))) {
            return result;
        }
        return result.thenApply(
                outcome -> {
                    if (outcome instanceof ZLinkStoreWriteApplied applied) {
                        events.emitAt(applied.storeNow(), "authorityWritten");
                    }
                    return outcome;
                });
    }

    private synchronized CompletionStage<ZLinkStoreWriteResult> writeLease(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        if (request.mutations().stream()
                .anyMatch(mutation -> mutation.key().equals(blockedLease))) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("injected owner lease termination"));
        }
        var stage = redis.write(request, cancellation);
        leaseWrites.add(stage);
        stage.whenComplete(
                (unused, failure) -> {
                    synchronized (this) {
                        leaseWrites.remove(stage);
                    }
                });
        return stage;
    }

    private CompletionStage<ZLinkStoreWriteResult> commit(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation, String event) {
        return writeBackend(request, cancellation)
                .thenApply(
                        result -> {
                            if (result instanceof ZLinkStoreWriteApplied applied) {
                                if (event.equals("closingCommitted")) ownerClosingCommitted = true;
                                if (event.equals("ownerDeleteCommitted")) {
                                    ownerClosingCommitted = false;
                                    // Another process can observe Missing before Redis's reply
                                    // reaches us.
                                    events.emitAt(applied.storeNow(), event);
                                } else {
                                    events.emit(event);
                                }
                            }
                            return result;
                        });
    }

    private CompletionStage<ZLinkStoreWriteResult> release(
            ZLinkStoreWriteRequest request,
            ZLinkStoreCancellation cancellation,
            Authority authority) {
        String mesh = text(authority.row().path("allocation").path("descriptor"), "meshName");
        var descriptor = key("mesh-node", mesh, authority.ridHex());
        var capacity = key("zlink:v12:capacity", mesh, authority.ridHex(), "actors", "active");
        var version =
                request.conditions().stream()
                        .filter(
                                condition ->
                                        condition instanceof ZLinkStoreVersionCondition item
                                                && item.key().equals(actorKey))
                        .map(
                                condition ->
                                        ((ZLinkStoreVersionCondition) condition).expected().value())
                        .findFirst()
                        .orElse("none");
        long delta =
                request.mutations().stream()
                        .filter(
                                mutation ->
                                        mutation instanceof ZLinkStoreAdd
                                                && mutation.key().equals(capacity))
                        .mapToLong(mutation -> ((ZLinkStoreAdd) mutation).delta())
                        .sum();
        boolean policy =
                request.conditions().stream()
                        .flatMap(condition -> condition.keys().stream())
                        .anyMatch(
                                key ->
                                        key.value().startsWith("mesh-node\0")
                                                && !key.equals(descriptor));
        return redis.read(actorKey, cancellation)
                .thenCombine(redis.read(authority.leaseKey(), cancellation), ReleaseReads::new)
                .thenCompose(
                        reads -> {
                            boolean invalidLease =
                                    conditioned(request, authority.leaseKey())
                                            && !leaseState(authority, reads.lease())
                                                    .equals("valid");
                            // Entry membership is carried by the exact Closing authority that is
                            // deleted.
                            boolean membership =
                                    reads.authority() instanceof ZLinkStoreReadFound found
                                            && found.value().version().value().equals(version)
                                            && entryClosing(found.value(), authority);
                            if (scenario.equals("f") && invalidLease)
                                events.emit("callerObservedClosingForRelease");
                            return writeBackend(request, cancellation)
                                    .thenApply(
                                            result -> {
                                                if (result
                                                        instanceof ZLinkStoreWriteApplied applied) {
                                                    // Redis applies negative Add only if the active
                                                    // count can cover it.
                                                    boolean capacityChecked =
                                                            conditioned(request, capacity)
                                                                    || (delta < 0
                                                                            && applied.putVersions()
                                                                                    .containsKey(
                                                                                            capacity));
                                                    events.emit(
                                                            "releaseCommitted",
                                                            "version",
                                                            version,
                                                            "invalidLease",
                                                            invalidLease,
                                                            "descriptorCondition",
                                                            conditioned(request, descriptor),
                                                            "capacityCondition",
                                                            capacityChecked,
                                                            "membershipRemoved",
                                                            membership,
                                                            "capacityDelta",
                                                            delta,
                                                            "relocationPolicyCondition",
                                                            policy);
                                                }
                                                return result;
                                            });
                        });
    }

    private static boolean entryClosing(ZLinkStoreValue value, Authority original) {
        Authority current = decode(value);
        return current.closing()
                && current.generation().equals(original.generation())
                && current.payload().spotKind().equals("entry");
    }

    private record ReleaseReads(ZLinkStoreReadResult authority, ZLinkStoreReadResult lease) {}

    private static boolean conditioned(ZLinkStoreWriteRequest request, ZLinkStoreKey key) {
        return request.conditions().stream().anyMatch(condition -> condition.keys().contains(key));
    }

    private static Authority decode(ZLinkStoreValue value) {
        JsonNode row = json(value.bytes());
        var payload = actorPayload(row);
        return new Authority(value.version().value(), row, payload);
    }

    record Authority(String version, JsonNode row, CrossLanguageOutageStore.ActorPayload payload) {
        int state() {
            return payload.state();
        }

        int operation() {
            return payload.operation();
        }

        String generation() {
            return text(row, "objectGeneration");
        }

        String ownerId() {
            return text(row, "ownerId");
        }

        String ownerGeneration() {
            return text(row, "authorityOwnerGeneration");
        }

        String leaseGeneration() {
            return text(row, "ownerLeaseGeneration");
        }

        String nodeGeneration() {
            return text(row.path("allocation"), "descriptorLifecycleGeneration");
        }

        String ridHex() {
            return text(row.path("allocation").path("descriptor"), "routingIdHex");
        }

        ZLinkStoreKey leaseKey() {
            return key("owner-lease", ownerId());
        }

        boolean closing() {
            return state() == 2 && operation() == 3;
        }

        boolean matchesLease(ZLinkStoreValue value) {
            var lease = json(value.bytes());
            return ownerId().equals(text(lease, "ownerId"))
                    && leaseGeneration().equals(text(lease, "leaseGeneration"));
        }
    }

    static <T> T await(CompletionStage<T> stage) {
        return stage.toCompletableFuture().join();
    }

    @Override
    public void close() {
        cancelHolds();
        redis.close();
    }

    static final class InjectedClosingCasFailure extends IllegalStateException {
        private InjectedClosingCasFailure() {
            super("injectedClosingCasFailure: injected Actor Closing CAS failure");
        }
    }

    private static final class SubmissionGate {
        private CompletableFuture<Void> release;

        synchronized void arm() {
            if (release != null) throw new IllegalStateException("submission is already held");
            release = new CompletableFuture<>();
        }

        synchronized CompletionStage<Void> beforeSubmission(Runnable held) {
            if (release == null) return CompletableFuture.completedFuture(null);
            held.run();
            return release;
        }

        void release(Runnable beforeRelease, Throwable failure) {
            CompletableFuture<Void> pending;
            synchronized (this) {
                if (release == null) throw new IllegalStateException("submission is not held");
                pending = release;
                beforeRelease.run();
                release = null;
            }
            if (failure == null) pending.complete(null);
            else pending.completeExceptionally(failure);
        }

        synchronized void cancel() {
            if (release != null) {
                var pending = release;
                release = null;
                pending.cancel(false);
            }
        }
    }
}
