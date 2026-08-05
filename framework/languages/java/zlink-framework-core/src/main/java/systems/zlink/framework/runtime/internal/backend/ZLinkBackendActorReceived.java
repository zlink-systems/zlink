package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import java.util.Optional;

public record ZLinkBackendActorReceived(
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
    ZLinkInboundDispatchBudget.Lease inboundDispatchLease) implements AutoCloseable {
    public ZLinkBackendActorReceived {
        requestSeq = requestSeq == null ? Optional.empty() : requestSeq;
        acceptedJournalRecord = acceptedJournalRecord == null
            ? new byte[0]
            : acceptedJournalRecord.clone();
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

    @Override
    public byte[] acceptedJournalRecord() {
        return acceptedJournalRecord.clone();
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
