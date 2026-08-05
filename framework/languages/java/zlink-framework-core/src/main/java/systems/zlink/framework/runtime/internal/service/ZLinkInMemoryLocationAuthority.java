package systems.zlink.framework.runtime.internal.service;

import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Consumer;

/**
 * Deterministic Location authority provider used by the JVM runtime contract.
 * Payload bytes are opaque and never affect provider-side CAS decisions.
 */
final class ZLinkInMemoryLocationAuthority {
    private static final int MAX_PAYLOAD_BYTES = 1024 * 1024;

    private final Map<String, Snapshot> rows = new HashMap<>();
    private final List<Consumer<Change>> listeners = new ArrayList<>();
    private final java.util.function.Supplier<Instant> now;
    private long storeVersion;
    private long objectGeneration;
    private long ownerGeneration;
    private long sequence;

    public ZLinkInMemoryLocationAuthority() {
        this(Instant::now);
    }

    public ZLinkInMemoryLocationAuthority(
        java.util.function.Supplier<Instant> now) {
        this.now = Objects.requireNonNull(now, "now");
    }

    public synchronized Read read(String key) {
        requireKey(key);
        Snapshot snapshot = rows.get(key);
        return new Read(now.get(), snapshot == null ? null : snapshot.copy());
    }

    public synchronized CasResult compareExchange(
        String key,
        Expectation expectation,
        Mutation mutation) {
        requireKey(key);
        Objects.requireNonNull(expectation, "expectation");
        Objects.requireNonNull(mutation, "mutation");
        Snapshot current = rows.get(key);
        if (!expectation.matches(current)) {
            return CasResult.conflict(read(key));
        }

        long newStoreVersion = nextStoreVersion();
        Instant storeNow = now.get();
        if (mutation.kind() == MutationKind.DELETE) {
            if (current == null) {
                return CasResult.conflict(read(key));
            }
            rows.remove(key);
            publish(new Change(
                nextSequence(), key, newStoreVersion, ChangeKind.DELETED));
            return CasResult.deleted(newStoreVersion, storeNow);
        }

        byte[] payload = Objects.requireNonNull(mutation.payload(), "payload");
        if (payload.length > MAX_PAYLOAD_BYTES) {
            throw new IllegalArgumentException(
                "authority payload exceeds 1 MiB");
        }
        long nextObject;
        long nextOwner;
        switch (mutation.kind()) {
            case PRESERVE -> {
                if (current == null) {
                    return CasResult.conflict(read(key));
                }
                nextObject = current.objectGeneration();
                nextOwner = current.authorityOwnerGeneration();
            }
            case NEW_OWNER -> {
                if (current == null) {
                    return CasResult.conflict(read(key));
                }
                nextObject = current.objectGeneration();
                nextOwner = nextOwnerGeneration();
            }
            case NEW_OBJECT -> {
                if (current != null) {
                    return CasResult.conflict(read(key));
                }
                nextObject = nextObjectGeneration();
                nextOwner = nextOwnerGeneration();
            }
            case DELETE -> throw new IllegalStateException("delete handled above");
            default -> throw new IllegalStateException(
                "unsupported mutation: " + mutation.kind());
        }

        Snapshot stored = new Snapshot(
            key,
            newStoreVersion,
            nextObject,
            nextOwner,
            payload);
        rows.put(key, stored);
        publish(new Change(
            nextSequence(), key, newStoreVersion, ChangeKind.STORED));
        return CasResult.stored(storeNow, stored);
    }

    public synchronized AutoCloseable subscribe(Consumer<Change> listener) {
        Objects.requireNonNull(listener, "listener");
        listeners.add(listener);
        return () -> {
            synchronized (ZLinkInMemoryLocationAuthority.this) {
                listeners.remove(listener);
            }
        };
    }

    private void publish(Change change) {
        List.copyOf(listeners).forEach(listener -> listener.accept(change));
    }

    private long nextStoreVersion() {
        return increment(storeVersion, value -> storeVersion = value, "storeVersion");
    }

    private long nextObjectGeneration() {
        return increment(
            objectGeneration,
            value -> objectGeneration = value,
            "objectGeneration");
    }

    private long nextOwnerGeneration() {
        return increment(
            ownerGeneration,
            value -> ownerGeneration = value,
            "ownerGeneration");
    }

    private long nextSequence() {
        return increment(sequence, value -> sequence = value, "changeSequence");
    }

    private static long increment(
        long current,
        java.util.function.LongConsumer assign,
        String field) {
        if (current == Long.MAX_VALUE) {
            throw new IllegalStateException(field + " is exhausted");
        }
        long next = current + 1;
        assign.accept(next);
        return next;
    }

    private static void requireKey(String key) {
        if (key == null || key.isEmpty() || key.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                "authority key must be non-empty text without NUL");
        }
    }

    public record Snapshot(
        String key,
        long storeVersion,
        long objectGeneration,
        long authorityOwnerGeneration,
        byte[] payload) {
        public Snapshot {
            payload = payload.clone();
        }

        @Override
        public byte[] payload() {
            return payload.clone();
        }

        Snapshot copy() {
            return new Snapshot(
                key,
                storeVersion,
                objectGeneration,
                authorityOwnerGeneration,
                payload);
        }
    }

    public record Read(Instant storeNow, Snapshot snapshot) {
    }

    public record Expectation(boolean missing, long storeVersion) {
        public static Expectation expectMissing() {
            return new Expectation(true, 0);
        }

        public static Expectation version(long storeVersion) {
            if (storeVersion <= 0) {
                throw new IllegalArgumentException("storeVersion must be positive");
            }
            return new Expectation(false, storeVersion);
        }

        boolean matches(Snapshot current) {
            return missing
                ? current == null
                : current != null && current.storeVersion() == storeVersion;
        }
    }

    public record Mutation(MutationKind kind, byte[] payload) {
        public Mutation {
            Objects.requireNonNull(kind, "kind");
            payload = payload == null ? null : payload.clone();
        }

        public static Mutation preserve(byte[] payload) {
            return new Mutation(MutationKind.PRESERVE, payload);
        }

        public static Mutation newOwner(byte[] payload) {
            return new Mutation(MutationKind.NEW_OWNER, payload);
        }

        public static Mutation newObject(byte[] payload) {
            return new Mutation(MutationKind.NEW_OBJECT, payload);
        }

        public static Mutation delete() {
            return new Mutation(MutationKind.DELETE, null);
        }

        @Override
        public byte[] payload() {
            return payload == null ? null : payload.clone();
        }
    }

    public enum MutationKind {
        PRESERVE,
        NEW_OWNER,
        NEW_OBJECT,
        DELETE
    }

    public record CasResult(
        CasKind kind,
        Instant storeNow,
        Snapshot snapshot,
        long deletedStoreVersion,
        Read conflict) {
        static CasResult stored(Instant now, Snapshot snapshot) {
            return new CasResult(
                CasKind.STORED, now, snapshot.copy(), 0, null);
        }

        static CasResult deleted(long storeVersion, Instant now) {
            return new CasResult(
                CasKind.DELETED, now, null, storeVersion, null);
        }

        static CasResult conflict(Read current) {
            return new CasResult(
                CasKind.CONFLICT, current.storeNow(), null, 0, current);
        }
    }

    public enum CasKind {
        STORED,
        DELETED,
        CONFLICT
    }

    public record Change(
        long sequence,
        String key,
        long storeVersion,
        ChangeKind kind) {
    }

    public enum ChangeKind {
        STORED,
        DELETED
    }
}
