package systems.zlink.samples.supportchat.server.support.application;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLong;

public final class ConversationAllocator {
    private final AtomicLong sequence = new AtomicLong();
    private final ConversationSpotFactory spots;

    public ConversationAllocator(ConversationSpotFactory spots) {
        this.spots = spots;
    }

    public CompletionStage<String> allocate(
        String customerActorId,
        String customerDisplayName,
        String subject) {
        String conversationId = "supportchat-conversation-" + sequence.incrementAndGet();
        ConversationSpotFactory.StartRequest request = new ConversationSpotFactory.StartRequest(
            customerActorId, customerDisplayName, subject, System.currentTimeMillis());
        return spots.start(conversationId, request).thenApply(ignored -> conversationId);
    }
}
