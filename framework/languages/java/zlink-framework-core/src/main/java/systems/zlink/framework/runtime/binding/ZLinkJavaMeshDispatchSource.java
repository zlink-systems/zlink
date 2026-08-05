package systems.zlink.framework.runtime.binding;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.function.Function;
import java.util.function.IntConsumer;
import java.util.function.IntUnaryOperator;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.runtime.internal.binding.spot.Claim;
import systems.zlink.framework.runtime.internal.binding.spot.ClaimRecvResult;
import systems.zlink.framework.runtime.internal.binding.spot.DrainResult;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyBatch;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveBatch;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRequirements;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

/**
 * Owns native reusable dispatch batches and hides claim/buffer management from
 * the framework scheduler.
 */
final class ZLinkJavaMeshDispatchSource implements ZLinkJavaMeshDispatchPump.Source {
    private static final int RECEIVE_OK = 0;
    private static final int RECEIVE_NO_DATA = 201;
    private static final int RECEIVE_BUFFER_TOO_SMALL = 207;
    private static final int READY_CAPACITY = 64;
    private static final int MESSAGE_CAPACITY = 64;
    private static final int PART_CAPACITY = 256;
    private static final int BYTE_CAPACITY = 1024 * 1024;

    private final MeshNode node;
    private final java.util.Set<PendingClaim> pendingClaims =
        ConcurrentHashMap.newKeySet();
    private final AtomicBoolean closed = new AtomicBoolean();
    private ReadyBatch readyBatch = ReadyBatch.create(READY_CAPACITY);
    private ReceiveBatch receiveBatch =
        ReceiveBatch.create(MESSAGE_CAPACITY, PART_CAPACITY, BYTE_CAPACITY);

    ZLinkJavaMeshDispatchSource(MeshNode node) {
        this.node = Objects.requireNonNull(node, "node");
    }

    @Override
    public void setReadyHandler(IntUnaryOperator handler) {
        node.setReadyHandler(handler == null ? null : handler::applyAsInt);
    }

    @Override
    public boolean drain(
        int domains,
        Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver,
        IntConsumer released) {
        readyBatch.reset();
        DrainResult result = node.drainReady(domains, readyBatch, RecvFlags.DONT_WAIT);
        if (result.resultCode() == RECEIVE_NO_DATA) {
            return false;
        }
        if (result.resultCode() != RECEIVE_OK) {
            throw new IllegalStateException(
                "MeshNode ready drain failed: " + result.resultCode());
        }
        for (int index = 0; index < readyBatch.count(); index++) {
            ReadyRecord owner = readyBatch.at(index);
            Claim claim = readyBatch.takeClaim(index);
            PendingClaim pendingClaim = new PendingClaim(
                claim, owner.domain(), released);
            pendingClaims.add(pendingClaim);
            List<ZLinkMeshDispatchRecord> records;
            try {
                records = drainClaim(owner, claim);
            } catch (RuntimeException error) {
                pendingClaim.release();
                throw error;
            }
            try {
                for (ZLinkMeshDispatchRecord record : records) {
                    boolean accepted = false;
                    try {
                        receiver.apply(record);
                        accepted = true;
                    } finally {
                        if (!accepted) {
                            record.close();
                        }
                    }
                }
            } finally {
                // Each dispatch record owns retained message parts and copied
                // metadata, so application work no longer depends on the native
                // claim. Release it before awaiting handlers; nested outbound
                // requests need the pump to drain their completion records.
                pendingClaim.release();
            }
        }
        return result.hasResidue();
    }

    private List<ZLinkMeshDispatchRecord> drainClaim(
        ReadyRecord owner,
        Claim claim) {
        receiveBatch.reset();
        ClaimRecvResult result = claim.recvBatch(receiveBatch, RecvFlags.DONT_WAIT);
        if (result.resultCode() == RECEIVE_BUFFER_TOO_SMALL) {
            resizeReceiveBatch(result.required());
            result = claim.recvBatch(receiveBatch, RecvFlags.DONT_WAIT);
        }
        if (result.resultCode() != RECEIVE_OK) {
            throw new IllegalStateException(
                "MeshNode claim receive failed: " + result.resultCode());
        }
        List<ZLinkMeshDispatchRecord> records =
            new ArrayList<>(receiveBatch.count());
        for (int index = 0; index < receiveBatch.count(); index++) {
            records.add(new ZLinkMeshDispatchRecord(
                owner,
                receiveBatch.at(index),
                receiveBatch.retainMessage(index)));
        }
        return records;
    }

    private void resizeReceiveBatch(ReceiveRequirements required) {
        receiveBatch.close();
        receiveBatch = ReceiveBatch.create(
            exactCapacity(required.messageCount(), "message"),
            exactCapacity(required.partCount(), "part"),
            exactCapacity(required.byteCount(), "byte"));
    }

    private static int exactCapacity(long value, String label) {
        if (value <= 0 || value > Integer.MAX_VALUE) {
            throw new IllegalStateException(
                "invalid required " + label + " capacity: " + value);
        }
        return (int) value;
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        node.setReadyHandler(null);
        List.copyOf(pendingClaims).forEach(PendingClaim::release);
        receiveBatch.close();
        readyBatch.close();
    }

    private final class PendingClaim {
        private final Claim claim;
        private final int domain;
        private final IntConsumer released;
        private final AtomicBoolean active = new AtomicBoolean(true);

        private PendingClaim(Claim claim, int domain, IntConsumer released) {
            this.claim = claim;
            this.domain = domain;
            this.released = released;
        }

        private void release() {
            if (!active.compareAndSet(true, false)) {
                return;
            }
            pendingClaims.remove(this);
            claim.close();
            if (!closed.get()) {
                released.accept(domain);
            }
        }
    }
}
