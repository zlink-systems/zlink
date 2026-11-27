package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator.CompletionPublicationUnknownException;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator.CompletionVersionConflictException;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator.ExpectedParticipant;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator.RelocationDataLostException;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCompletionRecordCodec.Entry;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCompletionRecordCodec.Record;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCompletionRecordCodec.Source;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;

final class ZLinkTargetRelocationCompletions {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final String PENDING = "pending";
    static final String TERMINAL_RECEIVED = "terminalReceived";
    static final String ALREADY_TERMINAL = "alreadyTerminal";
    private static final String SOURCE_LEASE_EXPIRED = "sourceLeaseExpired";

    private final ZLinkSpotRetireControl.StageRequest request;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkLocationRepository authorities;
    private final ZLinkRelocationStore blobs;
    private final Consumer<Runnable> resubmit;
    private final Map<String, Map<Identity, AcceptedRequest>> budget;
    private final String relocationId;
    private final ZLinkLocationOwnerToken targetOwner;

    private record AcceptedRequest(Entry failure, CompletableFuture<Void> delivered) {}

    private record Identity(Source source, String operationId) {
        static Identity of(Entry entry) {
            return new Identity(entry.source(), entry.operationId());
        }
    }

    ZLinkTargetRelocationCompletions(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkLocationRepository authorities,
            ZLinkRelocationStore blobs,
            Consumer<Runnable> resubmit,
            List<Entry> failures) {
        this.request = request;
        this.coordinator = coordinator;
        this.authorities = authorities;
        this.blobs = Objects.requireNonNull(blobs, "blobs");
        this.resubmit = resubmit;
        this.relocationId = request.fence().aggregateId().toString();
        this.targetOwner =
                new ZLinkLocationOwnerToken(
                        request.targetOwnerId(), request.targetOwnerLeaseGeneration());
        budget = new HashMap<>();
        for (Entry failure : failures) {
            var objectBudget =
                    budget.computeIfAbsent(failure.objectKey(), key -> new LinkedHashMap<>());
            if (objectBudget.putIfAbsent(
                            Identity.of(failure),
                            new AcceptedRequest(failure, new CompletableFuture<>()))
                    != null) {
                throw new RelocationDataLostException("Duplicate accepted relocation request.");
            }
        }
        for (var entries : budget.values()) {
            ZLinkAggregateRelocationCoordinator.requireCompletionFits(
                    new Record(relocationId, List.of()),
                    entries.values().stream().map(AcceptedRequest::failure).toList());
        }
    }

    boolean hasRequests() {
        return !budget.isEmpty();
    }

    Map<String, Long> acceptedRequests() {
        Map<String, Long> counts = new HashMap<>();
        budget.forEach((key, entries) -> counts.put(key, (long) entries.size()));
        return counts;
    }

    CompletionStage<Void> complete(Entry entry, Function<Entry, CompletionStage<String>> send) {
        String key = entry.objectKey();
        var accepted = budget.get(key);
        Identity identity = Identity.of(entry);
        var request = accepted == null ? null : accepted.get(identity);
        if (request == null) {
            return CompletableFuture.failedFuture(
                    new RelocationDataLostException("Result has no source-accepted request."));
        }
        var delivery = request.delivered();
        return retry(() -> update(key, entry, PENDING))
                .thenCompose(ignored -> retry(() -> deliver(key, identity, send)))
                .thenCompose(status -> retry(() -> update(key, entry, status)))
                .handle(
                        (ignored, failure) -> {
                            if (failure == null) delivery.complete(null);
                            else delivery.completeExceptionally(failure);
                            return delivery;
                        })
                .thenCompose(Function.identity());
    }

    private CompletionStage<String> deliver(
            String key, Identity identity, Function<Entry, CompletionStage<String>> send) {
        return read(key)
                .thenCompose(snapshot -> coordinator.readCompletion(snapshot, blobs, OPEN))
                .thenCompose(
                        record -> {
                            Entry published =
                                    record.entries().stream()
                                            .filter(entry -> Identity.of(entry).equals(identity))
                                            .findFirst()
                                            .orElseThrow(
                                                    () ->
                                                            new RelocationDataLostException(
                                                                    "Published completion result is"
                                                                            + " missing."));
                            if (!published.delivery().equals(PENDING)) {
                                return CompletableFuture.completedFuture(published.delivery());
                            }
                            Source source = published.source();
                            return authorities
                                    .readOwnerLease(source.ownerId())
                                    .thenCompose(
                                            lease ->
                                                    lease instanceof ZLinkOwnerLeaseFound found
                                                                    && found.isLiveFor(
                                                                            source.ownerId(),
                                                                            Long.parseUnsignedLong(
                                                                                    source
                                                                                            .leaseGeneration()))
                                                            ? send.apply(published)
                                                            : CompletableFuture.completedFuture(
                                                                    SOURCE_LEASE_EXPIRED));
                        });
    }

    private CompletionStage<ZLinkAuthoritySnapshot> update(
            String key, Entry entry, String delivery) {
        return read(key)
                .thenCompose(
                        snapshot ->
                                coordinator
                                        .readCompletion(snapshot, blobs, OPEN)
                                        .thenCompose(
                                                previous ->
                                                        replace(
                                                                snapshot, previous, entry,
                                                                delivery)));
    }

    private CompletionStage<ZLinkAuthoritySnapshot> replace(
            ZLinkAuthoritySnapshot snapshot, Record previous, Entry entry, String delivery) {
        Identity identity = Identity.of(entry);
        Map<Identity, Entry> results = new HashMap<>();
        for (Entry existing : previous.entries()) results.put(Identity.of(existing), existing);
        Entry current = results.get(identity);
        if (current != null && (delivery.equals(PENDING) || !current.delivery().equals(PENDING))) {
            return CompletableFuture.completedFuture(snapshot);
        }
        if (current == null && !delivery.equals(PENDING)) {
            throw new RelocationDataLostException(
                    "Cannot acknowledge a missing completion result.");
        }
        String key = entry.objectKey();
        var accepted = budget.get(key);
        results.put(identity, current == null ? entry : withDelivery(current, delivery));
        List<Entry> unfinished = new ArrayList<>();
        Record candidate = ordered(accepted, results, unfinished);
        if (current == null) {
            try {
                ZLinkAggregateRelocationCoordinator.requireCompletionFits(candidate, unfinished);
            } catch (ZLinkFrameworkException oversized) {
                if (oversized.kind() != ZLinkFrameworkErrorKind.INTERNAL_FAILURE) throw oversized;
                results.put(identity, accepted.get(identity).failure());
                unfinished.clear();
                candidate = ordered(accepted, results, unfinished);
            }
        }
        return coordinator.publishCompletion(key, snapshot, blobs, candidate, unfinished, OPEN);
    }

    private Record ordered(
            Map<Identity, AcceptedRequest> accepted,
            Map<Identity, Entry> results,
            List<Entry> unfinished) {
        List<Entry> entries = new ArrayList<>(results.size());
        accepted.forEach(
                (identity, failure) -> {
                    Entry result = results.get(identity);
                    if (result == null) unfinished.add(failure.failure());
                    else entries.add(result);
                });
        if (entries.size() != results.size()) {
            throw new RelocationDataLostException("Stored result has no source-accepted request.");
        }
        return new Record(relocationId, entries);
    }

    private static Entry withDelivery(Entry entry, String delivery) {
        return new Entry(
                entry.objectKey(),
                entry.source(),
                entry.operationId(),
                entry.replyRouteId(),
                entry.reply(),
                delivery);
    }

    CompletionStage<Void> finish(
            List<ExpectedParticipant> participants, Supplier<CompletionStage<Void>> normalize) {
        var fence =
                new ZLinkAggregateFence(
                        request.fence().aggregateId(), request.fence().aggregateGeneration());
        var deliveries =
                budget.values().stream()
                        .flatMap(requests -> requests.values().stream())
                        .map(AcceptedRequest::delivered)
                        .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(deliveries)
                .thenCompose(
                        ignored ->
                                retry(
                                        () ->
                                                coordinator.readTargetAuthorities(
                                                        participants, fence, targetOwner, OPEN)))
                .thenCompose(
                        snapshots ->
                                retry(normalize)
                                        .thenCompose(
                                                ignored -> {
                                                    CompletionStage<Void> deletes =
                                                            CompletableFuture.completedFuture(null);
                                                    for (var snapshot : snapshots.values()) {
                                                        deletes =
                                                                deletes.thenCompose(
                                                                        unused ->
                                                                                retry(
                                                                                        () ->
                                                                                                coordinator
                                                                                                        .deleteCompletion(
                                                                                                                snapshot,
                                                                                                                blobs,
                                                                                                                OPEN)));
                                                    }
                                                    return deletes;
                                                }));
    }

    private CompletionStage<ZLinkAuthoritySnapshot> read(String key) {
        return coordinator.readCompletionAuthority(
                key,
                request.fence().aggregateId(),
                request.targetOwnerId(),
                request.targetOwnerLeaseGeneration(),
                OPEN);
    }

    private <T> CompletionStage<T> retry(Supplier<CompletionStage<T>> operation) {
        var result = new CompletableFuture<T>();
        attempt(operation, result);
        return result;
    }

    private <T> void attempt(Supplier<CompletionStage<T>> operation, CompletableFuture<T> result) {
        CompletableFuture.completedFuture(null)
                .thenCompose(ignored -> operation.get())
                .whenComplete(
                        (value, failure) -> {
                            if (failure == null) {
                                result.complete(value);
                                return;
                            }
                            Throwable cause = failure;
                            while (cause instanceof CompletionException && cause.getCause() != null)
                                cause = cause.getCause();
                            if (!retryable(cause)) {
                                result.completeExceptionally(cause);
                                return;
                            }
                            Throwable retryCause = cause;
                            resubmit.accept(
                                    () -> {
                                        if (coordinator.isTargetLeaseValid(targetOwner)) {
                                            attempt(operation, result);
                                        } else {
                                            result.completeExceptionally(retryCause);
                                        }
                                    });
                        });
    }

    private static boolean retryable(Throwable cause) {
        if (cause instanceof CompletionVersionConflictException) return true;
        Throwable classified =
                cause instanceof CompletionPublicationUnknownException ? cause.getCause() : cause;
        return classified instanceof ZLinkFrameworkException framework
                && (framework.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE
                        || framework.kind() == ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED);
    }
}
