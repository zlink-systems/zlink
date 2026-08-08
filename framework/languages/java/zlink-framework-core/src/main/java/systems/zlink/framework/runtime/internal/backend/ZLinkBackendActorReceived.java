package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import java.util.Optional;
import java.util.function.Supplier;

public final class ZLinkBackendActorReceived implements AutoCloseable {
    private final ZLinkBackendActorRef actor;
    private final RoutingId sourceNodeRid;
    private final RoutingId sourceSessionRid;
    private final Optional<Long> requestSeq;
    private final long requestId;
    private final int flags;
    private final Message message;
    private final boolean hasMore;
    private final Supplier<byte[]> acceptedJournalRecordSupplier;
    private final boolean acceptedJournalRecordAvailable;
    private volatile byte[] acceptedJournalRecord;
    private final String contentType;
    private final ZLinkInboundDispatchBudget.Lease inboundDispatchLease;

    public ZLinkBackendActorReceived(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Optional<Long> requestSeq,
        long requestId,
        int flags,
        Message message,
        boolean hasMore,
        byte[] acceptedJournalRecord,
        String contentType,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        this(
            actor, sourceNodeRid, sourceSessionRid, requestSeq, requestId,
            flags, message, hasMore,
            () -> acceptedJournalRecord == null ? new byte[0] : acceptedJournalRecord,
            acceptedJournalRecord != null && acceptedJournalRecord.length > 0,
            contentType, inboundDispatchLease);
        this.acceptedJournalRecord = acceptedJournalRecord == null
            ? new byte[0]
            : acceptedJournalRecord;
    }

    private ZLinkBackendActorReceived(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Optional<Long> requestSeq,
        long requestId,
        int flags,
        Message message,
        boolean hasMore,
        Supplier<byte[]> acceptedJournalRecordSupplier,
        boolean acceptedJournalRecordAvailable,
        String contentType,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        this.actor = java.util.Objects.requireNonNull(actor, "actor");
        this.sourceNodeRid = java.util.Objects.requireNonNull(
            sourceNodeRid, "sourceNodeRid");
        this.sourceSessionRid = sourceSessionRid;
        this.requestSeq = requestSeq == null ? Optional.empty() : requestSeq;
        this.requestId = requestId;
        this.flags = flags;
        this.message = java.util.Objects.requireNonNull(message, "message");
        this.hasMore = hasMore;
        this.acceptedJournalRecordSupplier = java.util.Objects.requireNonNull(
            acceptedJournalRecordSupplier, "acceptedJournalRecordSupplier");
        this.acceptedJournalRecordAvailable = acceptedJournalRecordAvailable;
        this.contentType = contentType;
        this.inboundDispatchLease = inboundDispatchLease;
    }

    public static ZLinkBackendActorReceived lazyJournal(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Optional<Long> requestSeq,
        long requestId,
        int flags,
        Message message,
        boolean hasMore,
        Supplier<byte[]> acceptedJournalRecord,
        String contentType,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        return new ZLinkBackendActorReceived(
            actor, sourceNodeRid, sourceSessionRid, requestSeq, requestId,
            flags, message, hasMore, acceptedJournalRecord, true, contentType,
            inboundDispatchLease);
    }

    public ZLinkBackendActorRef actor() { return actor; }
    public RoutingId sourceNodeRid() { return sourceNodeRid; }
    public RoutingId sourceSessionRid() { return sourceSessionRid; }
    public Optional<Long> requestSeq() { return requestSeq; }
    public long requestId() { return requestId; }
    public int flags() { return flags; }
    public Message message() { return message; }
    public boolean hasMore() { return hasMore; }
    public boolean hasAcceptedJournalRecord() {
        return acceptedJournalRecordAvailable;
    }
    public String contentType() { return contentType; }
    public ZLinkInboundDispatchBudget.Lease inboundDispatchLease() {
        return inboundDispatchLease;
    }

    /** Backward-compatible constructor without an inbound content type. */
    public ZLinkBackendActorReceived(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Optional<Long> requestSeq,
        long requestId,
        int flags,
        Message message,
        boolean hasMore,
        byte[] acceptedJournalRecord,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        this(
            actor,
            sourceNodeRid,
            sourceSessionRid,
            requestSeq,
            requestId,
            flags,
            message,
            hasMore,
            acceptedJournalRecord,
            null,
            inboundDispatchLease);
    }

    public ZLinkBackendActorReceived(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Optional<Long> requestSeq,
        long requestId,
        int flags,
        Message message,
        boolean hasMore) {
        this(
            actor,
            sourceNodeRid,
            sourceSessionRid,
            requestSeq,
            requestId,
            flags,
            message,
            hasMore,
            new byte[0],
            null,
            null);
    }

    public ZLinkBackendActorReceived(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Optional<Long> requestSeq,
        long requestId,
        int flags,
        Message message,
        boolean hasMore,
        byte[] acceptedJournalRecord) {
        this(
            actor,
            sourceNodeRid,
            sourceSessionRid,
            requestSeq,
            requestId,
            flags,
            message,
            hasMore,
            acceptedJournalRecord,
            null,
            null);
    }

    public byte[] acceptedJournalRecord() {
        byte[] materialized = acceptedJournalRecord;
        if (materialized != null) {
            return materialized;
        }
        synchronized (this) {
            if (acceptedJournalRecord == null) {
                acceptedJournalRecord = java.util.Objects.requireNonNull(
                    acceptedJournalRecordSupplier.get(),
                    "accepted journal supplier returned null");
            }
            return acceptedJournalRecord;
        }
    }

    public void closeAdmission() {
        if (inboundDispatchLease != null) {
            inboundDispatchLease.close();
        }
    }

    @Override
    public void close() {
        try {
            message.close();
        } finally {
            closeAdmission();
        }
    }
}
