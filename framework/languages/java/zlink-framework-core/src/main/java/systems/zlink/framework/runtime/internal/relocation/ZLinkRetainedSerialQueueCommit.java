package systems.zlink.framework.runtime.internal.relocation;

import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

/**
 * Framework-private access to the serial queue's two-phase relocation
 * terminal. The public queue contract keeps its existing commit signature;
 * relocation owners use this scope to delay source release until target ACK.
 */
public final class ZLinkRetainedSerialQueueCommit {
    private static final ThreadLocal<Capture> CURRENT = new ThreadLocal<>();

    private ZLinkRetainedSerialQueueCommit() {
    }

    public static Optional<Commit> retain(
        ZLinkAsyncSerialQueue queue,
        ZLinkAsyncSerialQueue.RelocationSeal seal) {
        Objects.requireNonNull(queue, "queue");
        if (CURRENT.get() != null) {
            throw new IllegalStateException(
                "nested retained serial queue commit is not supported");
        }
        Capture capture = new Capture();
        CURRENT.set(capture);
        Optional<List<ZLinkAsyncSerialQueue.QueuedRecord>> records;
        try {
            records = queue.commitRelocation(seal);
        } finally {
            CURRENT.remove();
        }
        if (records.isEmpty()) {
            if (capture.owner != null) {
                throw new IllegalStateException(
                    "serial queue published a retained terminal without records");
            }
            return Optional.empty();
        }
        if (capture.owner == null
            || capture.records != records.orElseThrow()) {
            throw new IllegalStateException(
                "serial queue did not publish its retained terminal");
        }
        return Optional.of(new Commit(
            records.orElseThrow(), capture.owner));
    }

    /** Called only by {@link ZLinkAsyncSerialQueue#commitRelocation}. */
    public static boolean capture(
        List<ZLinkAsyncSerialQueue.QueuedRecord> records,
        Owner owner) {
        Capture current = CURRENT.get();
        if (current == null) {
            return false;
        }
        if (current.owner != null) {
            throw new IllegalStateException(
                "serial queue retained terminal was published twice");
        }
        current.records = Objects.requireNonNull(records, "records");
        current.owner = Objects.requireNonNull(owner, "owner");
        return true;
    }

    /** Framework-private retained owner implemented by the serial queue. */
    public interface Owner {
        Object monitor();
        Cut cut();
        boolean matches(Cut cut);
        void establish(Cut cut);
        void finish(Cut cut);
        boolean canAbort();
        void abort();
        void complete();
    }

    public static final class Cut {
        private final Object owner;
        private final long epoch;
        private final List<ZLinkAsyncSerialQueue.QueuedRecord> records;

        private Cut(
            Object owner,
            long epoch,
            List<ZLinkAsyncSerialQueue.QueuedRecord> records) {
            this.owner = Objects.requireNonNull(owner, "owner");
            if (epoch < 0) {
                throw new IllegalArgumentException(
                    "retained queue cut epoch must be non-negative");
            }
            this.epoch = epoch;
            this.records = List.copyOf(records);
        }

        public List<ZLinkAsyncSerialQueue.QueuedRecord> records() {
            return records;
        }

        public boolean belongsTo(Object expectedOwner) {
            return owner == expectedOwner;
        }

        public long epoch() {
            return epoch;
        }
    }

    public static Cut cut(
        Object owner,
        long epoch,
        List<ZLinkAsyncSerialQueue.QueuedRecord> records) {
        return new Cut(owner, epoch, records);
    }

    public static final class Commit {
        private final List<ZLinkAsyncSerialQueue.QueuedRecord> records;
        private final Owner owner;
        private final AtomicBoolean completed = new AtomicBoolean();

        private Commit(
            List<ZLinkAsyncSerialQueue.QueuedRecord> records,
            Owner owner) {
            this.records = List.copyOf(records);
            this.owner = owner;
        }

        public List<ZLinkAsyncSerialQueue.QueuedRecord> records() {
            return records;
        }

        public Cut cut() {
            return owner.cut();
        }

        public boolean tryFinishCapture(Cut cut) {
            return finishCapture(List.of(this), List.of(cut));
        }

        public boolean tryEstablishDurableCut(Cut cut) {
            return establishDurableCut(List.of(this), List.of(cut));
        }

        public boolean tryEstablishAndFinishCapture(Cut cut) {
            return establishAndFinishCapture(List.of(this), List.of(cut));
        }

        /** Restores the retained entries when the relocation fails before the
         * one-way cutover has been accepted. */
        public boolean abort() {
            if (completed.get()) {
                return false;
            }
            return abortRetained(List.of(this));
        }

        public void complete() {
            if (completed.compareAndSet(false, true)) {
                owner.complete();
            }
        }
    }

    /**
     * Finishes several retained lanes at one owner-side linearization point.
     * Every epoch is checked while all lane monitors are held, so no lane can
     * detach while another lane accepts an unrecorded suffix.
     */
    public static boolean finishCapture(
        List<Commit> commits,
        List<Cut> cuts) {
        List<Commit> owners = List.copyOf(commits);
        List<Cut> snapshots = List.copyOf(cuts);
        if (owners.isEmpty() || owners.size() != snapshots.size()) {
            throw new IllegalArgumentException(
                "retained lane commits and cuts must have the same size");
        }
        return withLocks(owners, snapshots, 0, false);
    }

    public static boolean establishDurableCut(
        List<Commit> commits,
        List<Cut> cuts) {
        List<Commit> owners = List.copyOf(commits);
        List<Cut> snapshots = List.copyOf(cuts);
        if (owners.isEmpty() || owners.size() != snapshots.size()) {
            throw new IllegalArgumentException(
                "retained lane commits and cuts must have the same size");
        }
        return withLocks(owners, snapshots, 0, true);
    }

    /** Establishes and detaches one cut under the same lane-lock set. */
    public static boolean establishAndFinishCapture(
        List<Commit> commits,
        List<Cut> cuts) {
        List<Commit> owners = List.copyOf(commits);
        List<Cut> snapshots = List.copyOf(cuts);
        if (owners.isEmpty() || owners.size() != snapshots.size()) {
            throw new IllegalArgumentException(
                "retained lane commits and cuts must have the same size");
        }
        return withLocks(owners, snapshots, 0, true, true);
    }

    public static boolean abortRetained(List<Commit> commits) {
        List<Commit> owners = List.copyOf(commits);
        if (owners.isEmpty() || owners.stream().anyMatch(
                commit -> commit.completed.get())) {
            return false;
        }
        return withAbortLocks(owners, 0);
    }

    private static boolean withAbortLocks(
        List<Commit> commits,
        int index) {
        if (index == commits.size()) {
            if (commits.stream().anyMatch(
                    commit -> !commit.owner.canAbort())) {
                return false;
            }
            commits.forEach(commit -> commit.owner.abort());
            return true;
        }
        synchronized (commits.get(index).owner.monitor()) {
            return withAbortLocks(commits, index + 1);
        }
    }

    private static boolean withLocks(
        List<Commit> commits,
        List<Cut> cuts,
        int index,
        boolean establish) {
        return withLocks(commits, cuts, index, establish, false);
    }

    private static boolean withLocks(
        List<Commit> commits,
        List<Cut> cuts,
        int index,
        boolean establish,
        boolean finishAfterEstablish) {
        if (index == commits.size()) {
            for (int current = 0; current < commits.size(); current++) {
                if (!commits.get(current).owner.matches(cuts.get(current))) {
                    return false;
                }
            }
            for (int current = 0; current < commits.size(); current++) {
                if (establish) {
                    commits.get(current).owner.establish(cuts.get(current));
                }
                if (!establish || finishAfterEstablish) {
                    commits.get(current).owner.finish(cuts.get(current));
                }
            }
            return true;
        }
        synchronized (commits.get(index).owner.monitor()) {
            return withLocks(
                commits,
                cuts,
                index + 1,
                establish,
                finishAfterEstablish);
        }
    }

    private static final class Capture {
        private List<ZLinkAsyncSerialQueue.QueuedRecord> records;
        private Owner owner;
    }
}
