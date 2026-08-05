package systems.zlink.framework.runtime.internal.dispatch;

import java.util.List;
import java.util.Objects;
import systems.zlink.contracts.messaging.Message;

/**
 * Bounds one receive turn before the scheduler returns to its fairness loop.
 *
 * <p>The first record is always eligible. This preserves progress for one
 * record whose payload is larger than the byte limit; subsequent records are
 * admitted only while the count, byte, and elapsed-time limits remain open.
 * The caller owns the received records and must close or dispatch them even
 * when the budget stops the next receive.</p>
 */
public final class ZLinkReceiveBatchBudget {
    public static final int DEFAULT_MAX_MESSAGES = 64;
    public static final long DEFAULT_MAX_BYTES = 1_048_576L;
    public static final long DEFAULT_MAX_ELAPSED_NANOS = 2_000_000L;

    private final int maxMessages;
    private final long maxBytes;
    private final long maxElapsedNanos;
    private final long startedNanos;
    private int messageCount;
    private long byteCount;

    public ZLinkReceiveBatchBudget() {
        this(
            DEFAULT_MAX_MESSAGES,
            DEFAULT_MAX_BYTES,
            DEFAULT_MAX_ELAPSED_NANOS);
    }

    public ZLinkReceiveBatchBudget(
        int maxMessages,
        long maxBytes,
        long maxElapsedNanos) {
        if (maxMessages <= 0) {
            throw new IllegalArgumentException("maxMessages must be positive");
        }
        if (maxBytes <= 0) {
            throw new IllegalArgumentException("maxBytes must be positive");
        }
        if (maxElapsedNanos <= 0) {
            throw new IllegalArgumentException(
                "maxElapsedNanos must be positive");
        }
        this.maxMessages = maxMessages;
        this.maxBytes = maxBytes;
        this.maxElapsedNanos = maxElapsedNanos;
        this.startedNanos = System.nanoTime();
    }

    /**
     * Returns whether the next backend receive may be attempted.
     */
    public boolean canReceiveNext() {
        if (messageCount >= maxMessages) {
            return false;
        }
        if (messageCount == 0) {
            return true;
        }
        return byteCount < maxBytes
            && System.nanoTime() - startedNanos < maxElapsedNanos;
    }

    /**
     * Records one receive after the backend has returned it.
     */
    public void record(long bytes) {
        if (bytes < 0) {
            throw new IllegalArgumentException("bytes must not be negative");
        }
        if (messageCount >= maxMessages) {
            throw new IllegalStateException("receive batch message limit exceeded");
        }
        messageCount++;
        byteCount = saturatingAdd(byteCount, bytes);
    }

    public int messageCount() {
        return messageCount;
    }

    public long byteCount() {
        return byteCount;
    }

    public static long bytesOf(List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        long bytes = 0;
        for (Message part : parts) {
            bytes = saturatingAdd(bytes, part == null ? 0 : part.size());
        }
        return bytes;
    }

    public static long bytesOf(
        List<Message> parts,
        int applicationMetadataBytes,
        int acceptedJournalRecordBytes) {
        if (applicationMetadataBytes < 0 || acceptedJournalRecordBytes < 0) {
            throw new IllegalArgumentException("metadata sizes must not be negative");
        }
        return saturatingAdd(
            bytesOf(parts),
            saturatingAdd(applicationMetadataBytes, acceptedJournalRecordBytes));
    }

    private static long saturatingAdd(long left, long right) {
        if (Long.MAX_VALUE - left < right) {
            return Long.MAX_VALUE;
        }
        return left + right;
    }
}
