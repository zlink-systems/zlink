package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.zip.CRC32C;
import systems.zlink.contracts.core.RoutingId;

/**
 * Deep module for the direct relocation payload transfer (spec 28 §4.2-§4.3).
 *
 * <p>It owns everything a caller would otherwise need to know about moving a
 * captured payload from source memory to a target: the CRC-32C manifest, the
 * chunk split, the in-flight payload budget with its Phase-1 conservative
 * bound, and the target-side ordinal assembly with its explicit-failure
 * verification rules. Callers see four operations — {@link #manifest},
 * {@link #chunks}, {@link Budget} and {@link Assembler}.
 */
final class ZLinkRelocationPayloadTransfer {
    /**
     * Phase-1 conservative effective-budget bound. Until the Core exposes an
     * observation API for per-pipe applied HWM and accounted charge, the
     * effective budget is min(configured, this constant) — a value safe for
     * every pipe role floor (spec 28 §5.3 implementation note).
     */
    static final long CONSERVATIVE_BUDGET_BYTES = 4L * 1024 * 1024;

    private ZLinkRelocationPayloadTransfer() {
    }

    /** Builds the PREPARE manifest for one captured payload. */
    static ZLinkCanonicalRelocationProtocol.Manifest manifest(
        byte[] payload,
        int chunkLimitBytes) {
        Objects.requireNonNull(payload, "payload");
        if (chunkLimitBytes <= 0) {
            throw new IllegalArgumentException(
                "relocation chunk limit must be positive");
        }
        int chunkCount = payload.length == 0
            ? 0
            : (payload.length + chunkLimitBytes - 1) / chunkLimitBytes;
        return new ZLinkCanonicalRelocationProtocol.Manifest(
            payload.length,
            chunkCount,
            crc32c(payload));
    }

    /** Splits the payload into ordinal-ordered chunks of the given size. */
    static List<byte[]> chunks(byte[] payload, int chunkLimitBytes) {
        Objects.requireNonNull(payload, "payload");
        List<byte[]> result = new ArrayList<>();
        for (int offset = 0; offset < payload.length;
            offset += chunkLimitBytes) {
            result.add(Arrays.copyOfRange(
                payload,
                offset,
                Math.min(payload.length, offset + chunkLimitBytes)));
        }
        return result;
    }

    static long crc32c(byte[] payload) {
        CRC32C checksum = new CRC32C();
        checksum.update(payload, 0, payload.length);
        return checksum.getValue();
    }

    /**
     * Phase-1 in-flight payload budget (spec 28 §5.3). Charges are counted
     * from chunk submission to its transport terminal. A full budget delays
     * the next chunk and new unit admission; it never fails a started
     * relocation, and a payload larger than the budget still flows because
     * one chunk never exceeds the effective budget.
     */
    static final class Budget {
        private final long peerBudgetBytes;
        private final long nodeBudgetBytes;
        private final int effectiveChunkBytes;
        private final Map<RoutingId, Long> peerInFlight = new HashMap<>();
        private long nodeInFlight;
        private final ArrayDeque<Waiter> waiters = new ArrayDeque<>();

        Budget(
            long chunkLimitBytes,
            long peerBudgetBytes,
            long nodeBudgetBytes) {
            if (chunkLimitBytes <= 0
                || peerBudgetBytes < 0
                || nodeBudgetBytes < 0) {
                throw new IllegalArgumentException(
                    "relocation transfer budget configuration is invalid");
            }
            this.peerBudgetBytes = effectiveBudget(peerBudgetBytes);
            this.nodeBudgetBytes = effectiveBudget(nodeBudgetBytes);
            long chunk = chunkLimitBytes;
            if (this.peerBudgetBytes > 0) {
                chunk = Math.min(chunk, this.peerBudgetBytes);
            }
            if (this.nodeBudgetBytes > 0) {
                chunk = Math.min(chunk, this.nodeBudgetBytes);
            }
            this.effectiveChunkBytes = (int) Math.min(
                chunk, Integer.MAX_VALUE);
        }

        private static long effectiveBudget(long configured) {
            return configured == 0
                ? 0
                : Math.min(configured, CONSERVATIVE_BUDGET_BYTES);
        }

        /** The effective chunk size after all budget bounds. */
        int effectiveChunkBytes() {
            return effectiveChunkBytes;
        }

        /**
         * Waits until a new relocation unit may apply its source admission
         * seal — the seal-side wait of spec 28 §5.3. The unit keeps handling
         * messages while it waits.
         */
        CompletionStage<Void> awaitUnitAdmission() {
            return acquire(null, 0);
        }

        /** Waits for headroom, then charges the chunk submission. */
        CompletionStage<Void> acquire(RoutingId peer, int bytes) {
            if (bytes < 0) {
                throw new IllegalArgumentException(
                    "budget charge must not be negative");
            }
            synchronized (this) {
                if (hasHeadroom(peer, bytes)) {
                    charge(peer, bytes);
                    return CompletableFuture.completedFuture(null);
                }
                Waiter waiter = new Waiter(peer, bytes);
                waiters.addLast(waiter);
                return waiter.admitted;
            }
        }

        /** Releases the charge after the chunk submission terminal. */
        void release(RoutingId peer, int bytes) {
            List<Waiter> admitted = new ArrayList<>();
            synchronized (this) {
                charge(peer, -bytes);
                var iterator = waiters.iterator();
                while (iterator.hasNext()) {
                    Waiter waiter = iterator.next();
                    if (hasHeadroom(waiter.peer, waiter.bytes)) {
                        charge(waiter.peer, waiter.bytes);
                        iterator.remove();
                        admitted.add(waiter);
                    }
                }
            }
            for (Waiter waiter : admitted) {
                waiter.admitted.complete(null);
            }
        }

        private boolean hasHeadroom(RoutingId peer, int bytes) {
            long peerCurrent = peer == null
                ? 0L
                : peerInFlight.getOrDefault(peer, 0L);
            boolean peerFits = peer == null
                || peerBudgetBytes == 0
                || peerCurrent == 0
                || peerCurrent + bytes <= peerBudgetBytes;
            boolean nodeFits = nodeBudgetBytes == 0
                || nodeInFlight == 0
                || nodeInFlight + bytes <= nodeBudgetBytes;
            return peerFits && nodeFits;
        }

        private void charge(RoutingId peer, long bytes) {
            if (peer != null) {
                long next = Math.max(
                    0L, peerInFlight.getOrDefault(peer, 0L) + bytes);
                if (next == 0) {
                    peerInFlight.remove(peer);
                } else {
                    peerInFlight.put(peer, next);
                }
            }
            nodeInFlight = Math.max(0L, nodeInFlight + bytes);
        }

        private record Waiter(
            RoutingId peer,
            int bytes,
            CompletableFuture<Void> admitted) {
            Waiter(RoutingId peer, int bytes) {
                this(peer, bytes, new CompletableFuture<>());
            }
        }
    }

    /**
     * Target-side ordinal assembly for one exact relocation identity
     * (spec 28 §4.3). Every accepted chunk is copied immediately; the
     * assembled payload is verified against the PREPARE manifest and a
     * mismatch is an explicit, non-retriable failure — never a partial
     * restore.
     */
    static final class Assembler {
        private final ZLinkCanonicalRelocationProtocol.Manifest manifest;
        private final byte[][] chunks;
        private int receivedChunks;
        private long receivedBytes;
        private final CompletableFuture<byte[]> assembled =
            new CompletableFuture<>();

        Assembler(ZLinkCanonicalRelocationProtocol.Manifest manifest) {
            this.manifest = Objects.requireNonNull(manifest, "manifest");
            this.chunks = new byte[manifest.chunkCount()][];
            if (manifest.chunkCount() == 0) {
                completeEmpty();
            }
        }

        /** The assembled and checksum-verified payload. */
        CompletionStage<byte[]> assembled() {
            return assembled;
        }

        /**
         * Accepts one state chunk. An identical duplicate is idempotent; a
         * conflicting duplicate or out-of-range ordinal is an explicit
         * failure of the whole assembly.
         */
        synchronized void accept(long chunkOrdinal, byte[] chunkData) {
            Objects.requireNonNull(chunkData, "chunkData");
            if (assembled.isCompletedExceptionally()) {
                return;
            }
            if (chunkOrdinal < 0 || chunkOrdinal >= chunks.length) {
                fail("relocation state chunk ordinal is outside the manifest");
                return;
            }
            int ordinal = (int) chunkOrdinal;
            byte[] existing = chunks[ordinal];
            if (existing != null) {
                if (!Arrays.equals(existing, chunkData)) {
                    fail("relocation state chunk conflicts with"
                        + " an already staged chunk");
                }
                return;
            }
            //  Copy immediately so the transport buffer and its Core lease
            //  can be released right after this call returns (spec 28 §4.3).
            chunks[ordinal] = chunkData.clone();
            receivedChunks++;
            receivedBytes += chunkData.length;
            if (receivedChunks == chunks.length) {
                verifyAndComplete();
            }
        }

        /**
         * Discards partially staged chunks so a retransmitted batch replaces
         * the whole section (spec 28 §4.4). A completed assembly is kept.
         */
        synchronized void discardPartial() {
            if (assembled.isDone()) {
                return;
            }
            Arrays.fill(chunks, null);
            receivedChunks = 0;
            receivedBytes = 0;
        }

        private void completeEmpty() {
            if (manifest.totalLength() != 0
                || manifest.checksumCrc32c() != crc32c(new byte[0])) {
                fail("relocation manifest of an empty payload is invalid");
                return;
            }
            assembled.complete(new byte[0]);
        }

        private void verifyAndComplete() {
            if (receivedBytes != manifest.totalLength()) {
                fail("assembled relocation payload length differs"
                    + " from the manifest");
                return;
            }
            byte[] payload = new byte[(int) receivedBytes];
            int offset = 0;
            for (byte[] chunk : chunks) {
                System.arraycopy(chunk, 0, payload, offset, chunk.length);
                offset += chunk.length;
            }
            if (crc32c(payload) != manifest.checksumCrc32c()) {
                //  A CRC-32C mismatch over TCP signals a defect, not a
                //  transient loss — no retry, no partial restore.
                fail("assembled relocation payload checksum differs"
                    + " from the manifest");
                return;
            }
            assembled.complete(payload);
        }

        private void fail(String message) {
            assembled.completeExceptionally(
                new IllegalStateException(message));
        }
    }

    /** Startup-only transfer configuration snapshot. */
    record Options(
        long chunkLimitBytes,
        long inFlightPayloadBudgetBytes,
        long nodeInFlightPayloadBudgetBytes,
        Duration cutoverWaitTimeout,
        Duration messageFollowDuration) {
        Options {
            if (chunkLimitBytes <= 0
                || inFlightPayloadBudgetBytes < 0
                || nodeInFlightPayloadBudgetBytes < 0) {
                throw new IllegalArgumentException(
                    "relocation transfer options are invalid");
            }
            Objects.requireNonNull(cutoverWaitTimeout, "cutoverWaitTimeout");
            Objects.requireNonNull(
                messageFollowDuration, "messageFollowDuration");
            if (cutoverWaitTimeout.isZero()
                || cutoverWaitTimeout.isNegative()
                || messageFollowDuration.isNegative()) {
                throw new IllegalArgumentException(
                    "relocation transfer timeouts are invalid");
            }
        }

        static Options defaults() {
            return new Options(
                262_144L,
                16_777_216L,
                0L,
                Duration.ofMillis(1_000),
                Duration.ofSeconds(30));
        }
    }
}
