package systems.zlink.framework.runtime.actors;
import java.util.Objects;

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
        byte[] journal = Objects.requireNonNull(
            acceptedJournalRecord, "acceptedJournalRecord");
        if (journal.length == 0) {
            throw new IllegalArgumentException(
                "accepted Actor handoff journal record is required");
        }
        this.acceptedJournalRecord = journal.clone();
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

    long retainedBytes() {
        return payload.size() + acceptedJournalRecord.length + 128L;
    }

    CompletionStage<Optional<Message>> reply() {
        return reply;
    }

    void complete(Optional<Message> response) {
        reply.complete(response);
    }

    boolean fail(Throwable error) {
        return reply.completeExceptionally(error);
    }

    @Override
    public void close() {
        payload.close();
    }

}
