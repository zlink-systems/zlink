package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.util.Optional;
import java.util.function.Consumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

public record ZLinkBackendReceived(
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
    ZLinkInboundDispatchBudget.Lease inboundDispatchLease) implements AutoCloseable {
    public ZLinkBackendReceived {
        result = result == null ? ZLinkBackendRequestResult.OK : result;
        applicationMetadata =
            applicationMetadata == null ? new byte[0] : applicationMetadata.clone();
        acceptedJournalRecord = acceptedJournalRecord == null
            ? new byte[0]
            : acceptedJournalRecord.clone();
    }

    public int applicationMetadataSize() {
        return applicationMetadata.length;
    }

    public int acceptedJournalRecordSize() {
        return acceptedJournalRecord.length;
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

    @Override
    public byte[] applicationMetadata() {
        return applicationMetadata.clone();
    }

    @Override
    public byte[] acceptedJournalRecord() {
        return acceptedJournalRecord.clone();
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
