package systems.zlink.framework.runtime;
import java.util.Arrays;
import systems.zlink.framework.locationprovider.ZLinkBlobFound;
import systems.zlink.framework.locationprovider.ZLinkBlobMissing;
import systems.zlink.framework.locationprovider.ZLinkBlobPutResult;
import systems.zlink.framework.locationprovider.ZLinkBlobReadResult;
import systems.zlink.framework.locationprovider.ZLinkBlobReference;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewResult;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewed;
import systems.zlink.framework.locationprovider.ZLinkBlobStored;

import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationDeleteResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationFound;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationMissing;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationReadResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewMissing;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewed;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStored;

import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.CRC32C;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

public final class InMemoryRelocationStore implements
    ZLinkRelocationStore,
    systems.zlink.framework.locationprovider.ZLinkRelocationStore {
    private final Map<String, Entry> values = new ConcurrentHashMap<>();
    private final AtomicReference<PutGate> nextInternalPut =
        new AtomicReference<>();

    public InMemoryRelocationStore() {
    }

    @Override
    public CompletionStage<ZLinkRelocationStored> put(
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        PutGate gate = nextInternalPut.getAndSet(null);
        if (gate != null) {
            gate.entered().complete(null);
            return gate.release().thenCompose(ignored ->
                put(payload, retention, cancellation));
        }
        String reference = UUID.randomUUID().toString();
        Instant now = Instant.now();
        Instant expiresAt = now.plus(retention);
        byte[] copy = payload.clone();
        values.put(reference, new Entry(copy, expiresAt));
        CRC32C checksum = new CRC32C();
        checksum.update(copy);
        return CompletableFuture.completedFuture(
            new ZLinkRelocationStored(
                reference, checksum.getValue(), expiresAt, now));
    }

    public void gateNextInternalPut(
        CompletableFuture<Void> entered,
        CompletableFuture<Void> release) {
        if (!nextInternalPut.compareAndSet(
                null,
                new PutGate(entered, release))) {
            throw new IllegalStateException(
                "an internal relocation put gate is already installed");
        }
    }

    @Override
    public CompletionStage<
        ZLinkBlobPutResult> put(
        ZLinkBlobReference reference,
        byte[] payload,
        Duration retention,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        Instant now = Instant.now();
        Instant expiresAt = now.plus(retention);
        Entry candidate = new Entry(payload, expiresAt);
        Entry current = values.putIfAbsent(reference.value(), candidate);
        if (current == null) {
            return CompletableFuture.completedFuture(
                new ZLinkBlobStored(
                    expiresAt, now));
        }
        return CompletableFuture.completedFuture(
            Arrays.equals(current.payload(), payload)
                ? new systems.zlink.framework.locationprovider
                    .ZLinkBlobAlreadyStored(current.expiresAt(), now)
                : new systems.zlink.framework.locationprovider
                    .ZLinkBlobConflict(now));
    }

    @Override
    public CompletionStage<
        ZLinkBlobReadResult> read(
        ZLinkBlobReference reference,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        Entry entry = values.get(reference.value());
        Instant now = Instant.now();
        if (entry == null || !entry.expiresAt().isAfter(now)) {
            values.remove(reference.value());
            return CompletableFuture.completedFuture(
                new ZLinkBlobMissing(now));
        }
        return CompletableFuture.completedFuture(
            new ZLinkBlobFound(
                entry.payload(), entry.expiresAt(), now));
    }

    @Override
    public CompletionStage<
        ZLinkBlobRenewResult> renew(
        ZLinkBlobReference reference,
        Duration retention,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        Entry entry = values.get(reference.value());
        Instant now = Instant.now();
        if (entry == null) {
            return CompletableFuture.completedFuture(
                new systems.zlink.framework.locationprovider
                    .ZLinkBlobRenewMissing(now));
        }
        Instant expiresAt = now.plus(retention);
        values.put(reference.value(), new Entry(entry.payload(), expiresAt));
        return CompletableFuture.completedFuture(
            new ZLinkBlobRenewed(
                expiresAt, now));
    }

    @Override
    public CompletionStage<Void> delete(
        ZLinkBlobReference reference,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        values.remove(reference.value());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<ZLinkRelocationReadResult> get(
        String reference,
        ZLinkStoreCancellation cancellation) {
        Entry entry = values.get(reference);
        Instant now = Instant.now();
        if (entry == null || !entry.expiresAt().isAfter(now)) {
            values.remove(reference);
            return CompletableFuture.completedFuture(
                new ZLinkRelocationMissing());
        }
        return CompletableFuture.completedFuture(
            new ZLinkRelocationFound(entry.payload()));
    }

    @Override
    public CompletionStage<ZLinkRelocationRenewResult> renew(
        String reference,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        Entry entry = values.get(reference);
        if (entry == null) {
            return CompletableFuture.completedFuture(
                new ZLinkRelocationRenewMissing());
        }
        Instant expiresAt = Instant.now().plus(retention);
        values.put(reference, new Entry(entry.payload(), expiresAt));
        return CompletableFuture.completedFuture(
            new ZLinkRelocationRenewed(expiresAt, Instant.now()));
    }

    @Override
    public CompletionStage<ZLinkRelocationDeleteResult> delete(
        String reference,
        ZLinkStoreCancellation cancellation) {
        return CompletableFuture.completedFuture(
            values.remove(reference) == null
                ? ZLinkRelocationDeleteResult.MISSING
                : ZLinkRelocationDeleteResult.DELETED);
    }

    private record Entry(byte[] payload, Instant expiresAt) {
        private Entry {
            payload = payload.clone();
        }
        @Override public byte[] payload() {
            return payload.clone();
        }
    }

    private record PutGate(
        CompletableFuture<Void> entered,
        CompletableFuture<Void> release) {
    }
}
