package systems.zlink.framework.locations.etcd;

import com.google.protobuf.ByteString;

import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.Rpc.*;
import systems.zlink.framework.locations.etcd.internal.proto.mvccpb.Kv.KeyValue;

import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Function;

public final class ZLinkEtcdLocationStore implements ZLinkLocationStore, AutoCloseable {
    private final EtcdClient client;
    private final String namespace;
    private final ByteString clock;
    private final EtcdCursor cursors = new EtcdCursor();
    private final ScheduledThreadPoolExecutor scheduler =
            new ScheduledThreadPoolExecutor(
                    1,
                    runnable -> {
                        Thread thread = new Thread(runnable, "zlink-etcd-cancellation");
                        thread.setDaemon(true);
                        return thread;
                    });
    private final Object lifetime = new Object();
    private int active;
    private boolean closing;
    private final CompletableFuture<Void> closed = new CompletableFuture<>();

    public ZLinkEtcdLocationStore(ZLinkEtcdLocationOptions options) {
        client = new EtcdClient(options);
        namespace = options.namespace();
        clock = EtcdEncoding.key(namespace, 'c', ByteString.EMPTY);
    }

    long scheduledCancellationTaskCount() {
        return scheduler.getTaskCount();
    }

    private ByteString metadataKey(ByteString key) {
        return EtcdEncoding.key(namespace, 'm', key);
    }

    private ByteString payloadKey(ByteString key) {
        return EtcdEncoding.key(namespace, 'p', key);
    }

    private static RequestOp readKey(ByteString key, long revision) {
        return RequestOp.newBuilder()
                .setRequestRange(RangeRequest.newBuilder().setKey(key).setRevision(revision))
                .build();
    }

    private static KeyValue one(RangeResponse response) {
        if (response.getKvsCount() > 1) throw EtcdEncoding.failure();
        return response.getKvsCount() == 0 ? null : response.getKvs(0);
    }

    private static long storeNow(TxnResponse response) {
        KeyValue value = one(response.getResponses(0).getResponseRange());
        if (value == null) throw EtcdEncoding.failure();
        return EtcdEncoding.decodeClock(value.getValue());
    }

    private static EtcdEncoding.Metadata record(KeyValue metadata, KeyValue payload) {
        if (metadata == null && payload == null) return null;
        if (metadata == null
                || payload == null
                || metadata.getModRevision() != payload.getModRevision())
            throw EtcdEncoding.failure();
        var info = EtcdEncoding.decodeMetadata(metadata.getValue());
        if (info.payloadLength() != payload.getValue().size()) throw EtcdEncoding.failure();
        return info;
    }

    private static ZLinkStoreValue value(
            KeyValue metadata,
            KeyValue payload,
            EtcdEncoding.Metadata info,
            long cluster,
            long now) {
        return new ZLinkStoreValue(
                payload.getValue().toByteArray(),
                new ZLinkStoreVersion(EtcdEncoding.version(cluster, metadata.getModRevision())),
                info.hasExpiry() ? Instant.ofEpochMilli(info.expiresAt()) : null,
                Instant.ofEpochMilli(now));
    }

    private <T> CompletionStage<T> run(
            ZLinkStoreCancellation cancellation,
            Function<EtcdClient.Operation, CompletableFuture<T>> work) {
        synchronized (lifetime) {
            if (closing)
                return CompletableFuture.failedFuture(
                        new IllegalStateException("Location Store is closed."));
            if (cancellation != null && cancellation.isCancellationRequested())
                return CompletableFuture.failedFuture(
                        new CancellationException("Store operation cancelled before I/O."));
            active++;
        }
        EtcdClient.Operation operation =
                new EtcdClient.Operation(client.timeoutNanos, cancellation, scheduler);
        CompletableFuture<T> pending;
        try {
            pending = work.apply(operation);
        } catch (RuntimeException error) {
            pending = CompletableFuture.failedFuture(error);
        }
        CompletableFuture<T> result = new CompletableFuture<>();
        pending.whenComplete(
                (response, error) -> {
                    if (error == null) {
                        try {
                            operation.check();
                        } catch (RuntimeException stopped) {
                            error = stopped;
                        }
                    }
                    operation.close();
                    synchronized (lifetime) {
                        active--;
                        lifetime.notifyAll();
                    }
                    if (error == null) result.complete(response);
                    else result.completeExceptionally(EtcdClient.unwrap(error));
                });
        return result;
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
        return run(
                cancellation,
                operation -> {
                    ByteString bytes =
                            EtcdEncoding.validateKey(key == null ? null : key.value(), false);
                    TxnRequest request =
                            TxnRequest.newBuilder()
                                    .addSuccess(readKey(clock, 0))
                                    .addSuccess(readKey(metadataKey(bytes), 0))
                                    .addSuccess(readKey(payloadKey(bytes), 0))
                                    .build();
                    return client.txn(request, operation)
                            .thenApply(
                                    response -> {
                                        long now = storeNow(response);
                                        KeyValue metadata =
                                                one(response.getResponses(1).getResponseRange());
                                        KeyValue payload =
                                                one(response.getResponses(2).getResponseRange());
                                        var info = record(metadata, payload);
                                        return info == null || info.expired(now)
                                                ? new ZLinkStoreReadMissing(
                                                        Instant.ofEpochMilli(now))
                                                : new ZLinkStoreReadFound(
                                                        value(
                                                                metadata,
                                                                payload,
                                                                info,
                                                                response.getHeader().getClusterId(),
                                                                now));
                                    });
                });
    }

    private static ZLinkStoreKey conditionKey(ZLinkStoreCondition condition) {
        return switch (condition) {
            case ZLinkStoreMissingCondition item -> item.key();
            case ZLinkStoreVersionCondition item -> item.key();
            case ZLinkStoreValueCondition item -> item.key();
            case null -> throw new IllegalArgumentException("A Store condition is required.");
        };
    }

    private static ZLinkStoreKey mutationKey(ZLinkStoreMutation mutation) {
        return switch (mutation) {
            case ZLinkStorePut item -> item.key();
            case ZLinkStoreDelete item -> item.key();
            case null -> throw new IllegalArgumentException("A Store mutation is required.");
        };
    }

    private static ByteString keyBytes(ZLinkStoreKey key) {
        return EtcdEncoding.validateKey(key == null ? null : key.value(), false);
    }

    private static void validate(ZLinkStoreWriteRequest request) {
        if (request == null) throw new IllegalArgumentException("A Store batch is required.");
        Set<ZLinkStoreKey> conditions = new HashSet<>(), mutations = new HashSet<>();
        long size = 0;
        for (var condition : request.conditions()) {
            var key = conditionKey(condition);
            size += keyBytes(key).size();
            if (!conditions.add(key))
                throw new IllegalArgumentException("Duplicate condition key.");
            if (condition instanceof ZLinkStoreVersionCondition version) {
                ByteString bytes =
                        EtcdEncoding.utf8(
                                version.expected() == null ? null : version.expected().value());
                if (bytes.isEmpty() || bytes.size() > 4096)
                    throw new IllegalArgumentException("Invalid Store version length.");
                size += bytes.size();
            }
            if (condition instanceof ZLinkStoreValueCondition value) {
                if (value.expected() == null || value.expected().length > EtcdEncoding.MAX_PAYLOAD)
                    throw new IllegalArgumentException("Invalid Store value length.");
                size += value.expected().length;
            }
        }
        for (var mutation : request.mutations()) {
            var key = mutationKey(mutation);
            size += keyBytes(key).size();
            if (!mutations.add(key)) throw new IllegalArgumentException("Duplicate mutation key.");
            if (mutation instanceof ZLinkStorePut put) {
                byte[] bytes = put.bytes();
                if (bytes.length > EtcdEncoding.MAX_PAYLOAD)
                    throw new IllegalArgumentException("Invalid Store value length.");
                EtcdEncoding.retention(put.retention());
                size += bytes.length;
            }
        }
        conditions.addAll(mutations);
        if (conditions.size() > 2048 || size > EtcdEncoding.MAX_PAGE_BYTES)
            throw new IllegalArgumentException("Store batch exceeds its bound.");
    }

    private static Compare exists(ByteString key, boolean exists) {
        return Compare.newBuilder()
                .setKey(key)
                .setTarget(Compare.CompareTarget.CREATE)
                .setResult(exists ? Compare.CompareResult.GREATER : Compare.CompareResult.EQUAL)
                .setCreateRevision(0)
                .build();
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        return run(
                cancellation,
                operation -> {
                    validate(request);
                    return writeAttempt(request, operation);
                });
    }

    private record Decision(
            long now,
            long cluster,
            long revision,
            KeyValue[] metadata,
            EtcdEncoding.Metadata[] info) {}

    private record Evaluation(boolean matches, long minimumExpiry) {}

    private CompletableFuture<ZLinkStoreWriteResult> writeAttempt(
            ZLinkStoreWriteRequest request, EtcdClient.Operation operation) {
        operation.check();
        return decide(request, operation)
                .thenCompose(decision -> readPayloads(request, decision, operation));
    }

    private CompletableFuture<Decision> decide(
            ZLinkStoreWriteRequest request, EtcdClient.Operation operation) {
        TxnRequest.Builder decision = TxnRequest.newBuilder().addSuccess(readKey(clock, 0));
        for (var condition : request.conditions())
            decision.addSuccess(readKey(metadataKey(keyBytes(conditionKey(condition))), 0));
        return client.txn(decision.build(), operation)
                .thenApply(
                        observed -> {
                            long now = storeNow(observed);
                            KeyValue[] metadata = new KeyValue[request.conditions().size()];
                            EtcdEncoding.Metadata[] info =
                                    new EtcdEncoding.Metadata[metadata.length];
                            for (int i = 0; i < metadata.length; i++) {
                                metadata[i] = one(observed.getResponses(i + 1).getResponseRange());
                                info[i] =
                                        metadata[i] == null
                                                ? null
                                                : EtcdEncoding.decodeMetadata(
                                                        metadata[i].getValue());
                            }
                            return new Decision(
                                    now,
                                    observed.getHeader().getClusterId(),
                                    observed.getHeader().getRevision(),
                                    metadata,
                                    info);
                        });
    }

    private CompletableFuture<ZLinkStoreWriteResult> readPayloads(
            ZLinkStoreWriteRequest request, Decision decision, EtcdClient.Operation operation) {
        TxnRequest.Builder payloadRequest = TxnRequest.newBuilder();
        List<Integer> positions = new ArrayList<>();
        for (int i = 0; i < decision.metadata().length; i++) {
            if (request.conditions().get(i) instanceof ZLinkStoreValueCondition condition
                    && decision.info()[i] != null
                    && decision.info()[i].payloadLength() == condition.expected().length) {
                positions.add(i);
                payloadRequest.addSuccess(
                        readKey(payloadKey(keyBytes(condition.key())), decision.revision()));
            }
        }
        CompletableFuture<TxnResponse> payloads =
                positions.isEmpty()
                        ? CompletableFuture.completedFuture(null)
                        : client.txn(payloadRequest.build(), operation);
        return payloads.handle(
                        (response, error) -> {
                            if (error != null) {
                                if (EtcdClient.unwrap(error)
                                                instanceof EtcdClient.CallFailure failure
                                        && failure.kind == EtcdClient.ErrorKind.COMPACTION)
                                    return writeAttempt(request, operation);
                                return CompletableFuture.<ZLinkStoreWriteResult>failedFuture(
                                        EtcdClient.unwrap(error));
                            }
                            Map<Integer, KeyValue> values = new HashMap<>();
                            for (int j = 0; j < positions.size(); j++) {
                                int i = positions.get(j);
                                KeyValue payload = one(response.getResponses(j).getResponseRange());
                                record(decision.metadata()[i], payload);
                                values.put(i, payload);
                            }
                            Evaluation evaluation = evaluate(request, decision, values);
                            if (!evaluation.matches())
                                return CompletableFuture.<ZLinkStoreWriteResult>completedFuture(
                                        new ZLinkStoreWriteConflict(
                                                Instant.ofEpochMilli(decision.now())));
                            return client.txn(
                                            applyRequest(
                                                    request, decision, evaluation.minimumExpiry()),
                                            operation)
                                    .thenCompose(
                                            responseApplied ->
                                                    responseApplied.getSucceeded()
                                                            ? CompletableFuture.completedFuture(
                                                                    applied(
                                                                            request,
                                                                            decision.now(),
                                                                            responseApplied))
                                                            : writeAttempt(request, operation));
                        })
                .thenCompose(value -> value);
    }

    private static Evaluation evaluate(
            ZLinkStoreWriteRequest request, Decision decision, Map<Integer, KeyValue> values) {
        long minimumExpiry = Long.MAX_VALUE;
        for (int i = 0; i < decision.metadata().length; i++) {
            var info = decision.info()[i];
            boolean live = info != null && !info.expired(decision.now());
            boolean matches =
                    switch (request.conditions().get(i)) {
                        case ZLinkStoreMissingCondition ignored -> !live;
                        case ZLinkStoreVersionCondition condition ->
                                live
                                        && Objects.equals(
                                                EtcdEncoding.decodeVersion(
                                                        condition.expected().value()),
                                                new EtcdEncoding.Version(
                                                        decision.cluster(),
                                                        decision.metadata()[i].getModRevision()));
                        case ZLinkStoreValueCondition condition ->
                                live
                                        && values.containsKey(i)
                                        && Arrays.equals(
                                                values.get(i).getValue().toByteArray(),
                                                condition.expected());
                    };
            if (!matches) return new Evaluation(false, minimumExpiry);
            if (live
                    && info.hasExpiry()
                    && !(request.conditions().get(i) instanceof ZLinkStoreMissingCondition))
                minimumExpiry = Math.min(minimumExpiry, info.expiresAt());
        }
        return new Evaluation(true, minimumExpiry);
    }

    private TxnRequest applyRequest(
            ZLinkStoreWriteRequest request, Decision decision, long minimumExpiry) {
        TxnRequest.Builder apply = TxnRequest.newBuilder().addCompare(exists(clock, true));
        if (minimumExpiry != Long.MAX_VALUE)
            apply.addCompare(
                    Compare.newBuilder()
                            .setKey(clock)
                            .setTarget(Compare.CompareTarget.VALUE)
                            .setResult(Compare.CompareResult.LESS)
                            .setValue(EtcdEncoding.clock(minimumExpiry)));
        for (int i = 0; i < decision.metadata().length; i++) {
            ByteString key = metadataKey(keyBytes(conditionKey(request.conditions().get(i))));
            apply.addCompare(
                    decision.metadata()[i] == null
                            ? exists(key, false)
                            : Compare.newBuilder()
                                    .setKey(key)
                                    .setTarget(Compare.CompareTarget.MOD)
                                    .setResult(Compare.CompareResult.EQUAL)
                                    .setModRevision(decision.metadata()[i].getModRevision())
                                    .build());
        }
        for (var mutation : request.mutations()) {
            ByteString key = keyBytes(mutationKey(mutation));
            if (mutation instanceof ZLinkStorePut put) {
                byte[] bytes = put.bytes();
                long expires =
                        put.retention() == null
                                ? 0
                                : decision.now() + EtcdEncoding.retention(put.retention());
                if (expires > EtcdEncoding.MAX_TIME)
                    throw new IllegalArgumentException("Store expiry exceeds its bound.");
                apply.addSuccess(
                        RequestOp.newBuilder()
                                .setRequestPut(
                                        PutRequest.newBuilder()
                                                .setKey(metadataKey(key))
                                                .setValue(
                                                        EtcdEncoding.metadata(
                                                                put.retention() != null,
                                                                expires,
                                                                bytes.length))));
                apply.addSuccess(
                        RequestOp.newBuilder()
                                .setRequestPut(
                                        PutRequest.newBuilder()
                                                .setKey(payloadKey(key))
                                                .setValue(ByteString.copyFrom(bytes))));
            } else {
                apply.addSuccess(
                        RequestOp.newBuilder()
                                .setRequestDeleteRange(
                                        DeleteRangeRequest.newBuilder().setKey(metadataKey(key))));
                apply.addSuccess(
                        RequestOp.newBuilder()
                                .setRequestDeleteRange(
                                        DeleteRangeRequest.newBuilder().setKey(payloadKey(key))));
            }
        }
        return apply.build();
    }

    private static ZLinkStoreWriteResult applied(
            ZLinkStoreWriteRequest request, long now, TxnResponse response) {
        Map<ZLinkStoreKey, ZLinkStoreVersion> versions = new LinkedHashMap<>();
        for (var mutation : request.mutations())
            if (mutation instanceof ZLinkStorePut put)
                versions.put(
                        put.key(),
                        new ZLinkStoreVersion(
                                EtcdEncoding.version(
                                        response.getHeader().getClusterId(),
                                        response.getHeader().getRevision())));
        return new ZLinkStoreWriteApplied(versions, Instant.ofEpochMilli(now));
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
        return run(
                cancellation,
                operation -> {
                    if (request == null || request.limit() < 1 || request.limit() > 1000)
                        throw new IllegalArgumentException("Invalid Store page limit.");
                    ByteString prefix = EtcdEncoding.validateKey(request.prefix(), true);
                    EtcdCursor.Snapshot snapshot = null;
                    if (request.cursor() != null) {
                        ByteString cursor = EtcdEncoding.utf8(request.cursor().value());
                        if (cursor.isEmpty() || cursor.size() > 4096)
                            throw new IllegalArgumentException("Invalid Store cursor length.");
                        snapshot = cursors.decode(request.cursor().value(), namespace, prefix);
                        if (snapshot == null)
                            return CompletableFuture.completedFuture(new ZLinkStoreScanExpired());
                    }
                    var previous = snapshot;
                    long started = System.nanoTime();
                    return client.txn(
                                    TxnRequest.newBuilder().addSuccess(readKey(clock, 0)).build(),
                                    operation)
                            .thenCompose(
                                    observed -> {
                                        long now = storeNow(observed);
                                        if (previous != null
                                                && previous.cluster()
                                                        != observed.getHeader().getClusterId())
                                            return CompletableFuture.completedFuture(
                                                    new ZLinkStoreScanExpired());
                                        var current =
                                                previous == null
                                                        ? new EtcdCursor.Snapshot(
                                                                observed.getHeader().getClusterId(),
                                                                observed.getHeader().getRevision(),
                                                                now,
                                                                started,
                                                                ByteString.EMPTY)
                                                        : previous;
                                        return scanMetadata(
                                                request,
                                                prefix,
                                                current,
                                                operation,
                                                new ArrayList<>(),
                                                0);
                                    })
                            .exceptionallyCompose(
                                    error -> {
                                        if (EtcdClient.unwrap(error)
                                                        instanceof EtcdClient.CallFailure failure
                                                && failure.kind == EtcdClient.ErrorKind.COMPACTION)
                                            return CompletableFuture.completedFuture(
                                                    new ZLinkStoreScanExpired());
                                        return CompletableFuture.failedFuture(
                                                EtcdClient.unwrap(error));
                                    });
                });
    }

    private record Selected(ByteString key, KeyValue metadata, EtcdEncoding.Metadata info) {}

    private CompletableFuture<ZLinkStoreScanResult> scanMetadata(
            ZLinkStoreScanRequest request,
            ByteString prefix,
            EtcdCursor.Snapshot snapshot,
            EtcdClient.Operation operation,
            List<Selected> selected,
            int size) {
        if (snapshot.expired())
            return CompletableFuture.completedFuture(new ZLinkStoreScanExpired());
        ByteString start =
                snapshot.last().isEmpty()
                        ? metadataKey(prefix)
                        : metadataKey(snapshot.last()).concat(ByteString.copyFrom(new byte[] {0}));
        RangeRequest rangeRequest =
                RangeRequest.newBuilder()
                        .setKey(start)
                        .setRangeEnd(EtcdEncoding.prefixEnd(metadataKey(prefix)))
                        .setRevision(snapshot.revision())
                        .setLimit(1000)
                        .setSortOrder(RangeRequest.SortOrder.ASCEND)
                        .setSortTarget(RangeRequest.SortTarget.KEY)
                        .build();
        return client.range(rangeRequest, operation)
                .thenCompose(
                        range ->
                                selectScanPage(
                                        request, prefix, snapshot, operation, selected, size,
                                        range));
    }

    private CompletableFuture<ZLinkStoreScanResult> selectScanPage(
            ZLinkStoreScanRequest request,
            ByteString prefix,
            EtcdCursor.Snapshot snapshot,
            EtcdClient.Operation operation,
            List<Selected> selected,
            int size,
            RangeResponse range) {
        if (range.getHeader().getClusterId() != snapshot.cluster())
            return CompletableFuture.completedFuture(new ZLinkStoreScanExpired());
        ByteString last = snapshot.last();
        int pageSize = size;
        boolean more = false;
        for (KeyValue metadata : range.getKvsList()) {
            ByteString key = metadata.getKey().substring(metadataKey(ByteString.EMPTY).size());
            EtcdEncoding.storedKey(key);
            var info = EtcdEncoding.decodeMetadata(metadata.getValue());
            if (info.expired(snapshot.now())) {
                last = key;
                continue;
            }
            int itemSize =
                    key.size()
                            + info.payloadLength()
                            + EtcdEncoding.version(snapshot.cluster(), metadata.getModRevision())
                                    .length()
                            + 128;
            if (selected.size() == request.limit()
                    || pageSize + itemSize > EtcdEncoding.MAX_PAGE_BYTES) {
                more = true;
                break;
            }
            pageSize += itemSize;
            selected.add(new Selected(key, metadata, info));
            last = key;
        }
        var advanced = snapshot.advance(last);
        if (!more && range.getMore()) {
            if (range.getKvsCount() == 0) throw EtcdEncoding.failure();
            return scanMetadata(request, prefix, advanced, operation, selected, pageSize);
        }
        return readScanPayloads(prefix, advanced, operation, selected, more);
    }

    private CompletableFuture<ZLinkStoreScanResult> readScanPayloads(
            ByteString prefix,
            EtcdCursor.Snapshot snapshot,
            EtcdClient.Operation operation,
            List<Selected> selected,
            boolean hasMore) {
        TxnRequest.Builder payloadRequest = TxnRequest.newBuilder();
        for (var item : selected)
            payloadRequest.addSuccess(readKey(payloadKey(item.key()), snapshot.revision()));
        CompletableFuture<TxnResponse> payloads =
                selected.isEmpty()
                        ? CompletableFuture.completedFuture(null)
                        : client.txn(payloadRequest.build(), operation);
        return payloads.thenApply(
                response -> scanPage(prefix, snapshot, selected, hasMore, response));
    }

    private ZLinkStoreScanResult scanPage(
            ByteString prefix,
            EtcdCursor.Snapshot snapshot,
            List<Selected> selected,
            boolean hasMore,
            TxnResponse response) {
        if (snapshot.expired()
                || (response != null && response.getHeader().getClusterId() != snapshot.cluster()))
            return new ZLinkStoreScanExpired();
        List<ZLinkStoreScanItem> items = new ArrayList<>();
        for (int i = 0; i < selected.size(); i++) {
            var item = selected.get(i);
            var payload = one(response.getResponses(i).getResponseRange());
            record(item.metadata(), payload);
            items.add(
                    new ZLinkStoreScanItem(
                            new ZLinkStoreKey(EtcdEncoding.storedKey(item.key())),
                            value(
                                    item.metadata(),
                                    payload,
                                    item.info(),
                                    snapshot.cluster(),
                                    snapshot.now())));
        }
        return new ZLinkStoreScanPageResult(
                new ZLinkStoreScanPage(
                        items,
                        hasMore
                                ? new ZLinkStoreScanCursor(
                                        cursors.encode(snapshot, namespace, prefix))
                                : null,
                        Instant.ofEpochMilli(snapshot.now())));
    }

    @Override
    public void close() {
        boolean owner;
        boolean interrupted = false;
        synchronized (lifetime) {
            owner = !closing;
            closing = true;
            if (owner)
                while (active != 0)
                    try {
                        lifetime.wait();
                    } catch (InterruptedException error) {
                        interrupted = true;
                    }
        }
        if (owner) {
            try {
                client.close();
                scheduler.shutdownNow();
                while (!scheduler.isTerminated()) {
                    try {
                        scheduler.awaitTermination(1, TimeUnit.SECONDS);
                    } catch (InterruptedException error) {
                        interrupted = true;
                    }
                }
                closed.complete(null);
            } catch (RuntimeException error) {
                closed.completeExceptionally(error);
                throw error;
            } finally {
                if (interrupted) Thread.currentThread().interrupt();
            }
        } else closed.join();
    }
}
