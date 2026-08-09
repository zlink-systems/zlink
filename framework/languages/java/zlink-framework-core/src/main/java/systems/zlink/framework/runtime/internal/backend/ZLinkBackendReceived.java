package systems.zlink.framework.runtime.internal.backend;
import java.util.Objects;

import java.util.List;
import java.util.Optional;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

public final class ZLinkBackendReceived implements AutoCloseable {
    private final ZLinkBackendRequestResult result;
    private final Optional<RoutingId> routingId;
    private final Optional<String> spotId;
    private final Optional<Long> requestSeq;
    private final byte[] applicationMetadata;
    private final Supplier<byte[]> acceptedJournalRecordSupplier;
    private final int acceptedJournalRecordSizeHint;
    private volatile byte[] acceptedJournalRecord;
    private final List<Message> parts;
    private final Consumer<List<Message>> reply;
    private final Runnable closeAction;
    private final String contentType;
    private final ZLinkInboundDispatchBudget.Lease inboundDispatchLease;

    public ZLinkBackendReceived(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        byte[] applicationMetadata,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction,
        String contentType,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        this(
            result,
            routingId,
            spotId,
            requestSeq,
            applicationMetadata,
            () -> acceptedJournalRecord == null ? new byte[0] : acceptedJournalRecord,
            acceptedJournalRecord == null ? 0 : acceptedJournalRecord.length,
            parts,
            reply,
            closeAction,
            contentType,
            inboundDispatchLease);
        this.acceptedJournalRecord = acceptedJournalRecord == null
            ? new byte[0]
            : acceptedJournalRecord;
    }

    private ZLinkBackendReceived(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        byte[] applicationMetadata,
        Supplier<byte[]> acceptedJournalRecordSupplier,
        int acceptedJournalRecordSizeHint,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction,
        String contentType,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        this.result = result == null ? ZLinkBackendRequestResult.OK : result;
        this.routingId = routingId == null ? Optional.empty() : routingId;
        this.spotId = spotId == null ? Optional.empty() : spotId;
        this.requestSeq = requestSeq == null ? Optional.empty() : requestSeq;
        this.applicationMetadata = applicationMetadata == null
            ? new byte[0]
            : applicationMetadata.clone();
        this.acceptedJournalRecordSupplier = Objects.requireNonNull(
            acceptedJournalRecordSupplier, "acceptedJournalRecordSupplier");
        this.acceptedJournalRecordSizeHint = Math.max(0, acceptedJournalRecordSizeHint);
        this.parts = Objects.requireNonNull(parts, "parts");
        this.reply = reply;
        this.closeAction = closeAction == null ? () -> { } : closeAction;
        this.contentType = contentType;
        this.inboundDispatchLease = inboundDispatchLease;
    }

    public static ZLinkBackendReceived lazyJournal(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        byte[] applicationMetadata,
        Supplier<byte[]> acceptedJournalRecord,
        int acceptedJournalRecordSizeHint,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction,
        String contentType,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        return new ZLinkBackendReceived(
            result, routingId, spotId, requestSeq, applicationMetadata,
            acceptedJournalRecord, acceptedJournalRecordSizeHint, parts, reply,
            closeAction, contentType, inboundDispatchLease);
    }

    public ZLinkBackendRequestResult result() { return result; }
    public Optional<RoutingId> routingId() { return routingId; }
    public Optional<String> spotId() { return spotId; }
    public Optional<Long> requestSeq() { return requestSeq; }
    public List<Message> parts() { return parts; }
    public Consumer<List<Message>> reply() { return reply; }
    public Runnable closeAction() { return closeAction; }
    public String contentType() { return contentType; }
    public ZLinkInboundDispatchBudget.Lease inboundDispatchLease() {
        return inboundDispatchLease;
    }

    public int applicationMetadataSize() {
        return applicationMetadata.length;
    }

    public int acceptedJournalRecordSize() {
        byte[] materialized = acceptedJournalRecord;
        return materialized == null
            ? acceptedJournalRecordSizeHint
            : materialized.length;
    }

    /** Backward-compatible constructor without an inbound content type. */
    public ZLinkBackendReceived(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        byte[] applicationMetadata,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        this(
            result,
            routingId,
            spotId,
            requestSeq,
            applicationMetadata,
            acceptedJournalRecord,
            parts,
            reply,
            closeAction,
            null,
            inboundDispatchLease);
    }

    public ZLinkBackendReceived(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        byte[] applicationMetadata,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction) {
        this(
            result,
            routingId,
            spotId,
            requestSeq,
            applicationMetadata,
            new byte[0],
            parts,
            reply,
            closeAction,
            null,
            null);
    }

    public ZLinkBackendReceived(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        byte[] applicationMetadata,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction) {
        this(
            result,
            routingId,
            spotId,
            requestSeq,
            applicationMetadata,
            acceptedJournalRecord,
            parts,
            reply,
            closeAction,
            null,
            null);
    }

    public ZLinkBackendReceived(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction) {
        this(
            result,
            routingId,
            spotId,
            requestSeq,
            new byte[0],
            new byte[0],
            parts,
            reply,
            closeAction,
            null,
            null);
    }

    public ZLinkBackendReceived(
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        List<Message> parts) {
        this(
            ZLinkBackendRequestResult.OK,
            routingId,
            spotId,
            requestSeq,
            new byte[0],
            new byte[0],
            parts,
            null,
            () -> { },
            null,
            null);
    }

    public ZLinkBackendReceived(
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        this(
            ZLinkBackendRequestResult.OK,
            routingId,
            spotId,
            requestSeq,
            new byte[0],
            new byte[0],
            parts,
            reply,
            () -> { },
            null,
            null);
    }

    public ZLinkBackendReceived(
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Runnable closeAction) {
        this(
            ZLinkBackendRequestResult.OK,
            routingId,
            spotId,
            requestSeq,
            new byte[0],
            new byte[0],
            parts,
            reply,
            closeAction,
            null,
            null);
    }

    public ZLinkBackendReceived(
        ZLinkBackendRequestResult result,
        Optional<RoutingId> routingId,
        Optional<String> spotId,
        Optional<Long> requestSeq,
        List<Message> parts) {
        this(
            result,
            routingId,
            spotId,
            requestSeq,
            new byte[0],
            new byte[0],
            parts,
            null,
            () -> { },
            null,
            null);
    }

    public byte[] applicationMetadata() {
        return applicationMetadata.clone();
    }

    public byte[] acceptedJournalRecord() {
        byte[] materialized = acceptedJournalRecord;
        if (materialized != null) {
            return materialized;
        }
        synchronized (this) {
            if (acceptedJournalRecord == null) {
                acceptedJournalRecord = Objects.requireNonNull(
                    acceptedJournalRecordSupplier.get(),
                    "accepted journal supplier returned null");
            }
            return acceptedJournalRecord;
        }
    }

    public void reply(List<Message> replyParts) {
        if (reply == null) {
            throw new IllegalStateException("received message has no reply path");
        }
        reply.accept(replyParts);
    }

    /** Releases the retained transport parts without releasing the dispatch lease. */
    public void closeParts() {
        parts.forEach(Message::close);
    }

    /** Releases the application admission lease retained by this receive. */
    public void closeAdmission() {
        if (inboundDispatchLease != null) {
            inboundDispatchLease.close();
        }
    }

    @Override
    public void close() {
        try {
            closeParts();
        } finally {
            try {
                closeAction.run();
            } finally {
                closeAdmission();
            }
        }
    }
}
