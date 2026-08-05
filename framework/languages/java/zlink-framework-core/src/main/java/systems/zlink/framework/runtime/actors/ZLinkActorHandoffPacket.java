package systems.zlink.framework.runtime.actors;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

final class ZLinkActorHandoffPacket implements AutoCloseable {
    private final long arrivalIndex;
    private final ZLinkStreamHeader header;
    private final Message payload;
    private final ZLinkActorReplyRoute replyRoute;
    private final byte[] acceptedJournalRecord;
    private final CompletableFuture<Optional<Message>> reply = new CompletableFuture<>();

    ZLinkActorHandoffPacket(
        long arrivalIndex,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute,
        byte[] acceptedJournalRecord) {
        this.arrivalIndex = arrivalIndex;
        this.header = header;
        this.payload = Message.from(payload);
        this.replyRoute = replyRoute;
        this.acceptedJournalRecord = java.util.Objects.requireNonNull(
            acceptedJournalRecord, "acceptedJournalRecord").clone();
        if (this.acceptedJournalRecord.length == 0) {
            throw new IllegalArgumentException(
                "accepted Actor handoff journal record is required");
        }
    }

    long arrivalIndex() {
        return arrivalIndex;
    }

    ZLinkStreamHeader header() {
        return header;
    }

    Message payload() {
        return payload;
    }

    ZLinkActorReplyRoute replyRoute() {
        return replyRoute;
    }

    byte[] acceptedJournalRecord() {
        return acceptedJournalRecord.clone();
    }

    CompletionStage<Optional<Message>> reply() {
        trace("reply-observed");
        return reply;
    }

    void complete(Optional<Message> response) {
        trace("reply-complete present=" + response.isPresent()
            + " completed=" + reply.complete(response));
    }

    boolean fail(Throwable error) {
        return reply.completeExceptionally(error);
    }

    @Override
    public void close() {
        payload.close();
    }

    private void trace(String detail) {
        if ("1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"))) {
            java.util.logging.Logger.getLogger(
                    ZLinkActorHandoffPacket.class.getName())
                .warning("[zlink-java-stream-trace] handoff packet "
                    + arrivalIndex + " " + detail);
        }
    }
}
