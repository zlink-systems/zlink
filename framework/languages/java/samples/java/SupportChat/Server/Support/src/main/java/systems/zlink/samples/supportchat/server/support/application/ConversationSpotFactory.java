package systems.zlink.samples.supportchat.server.support.application;

import java.util.concurrent.CompletionStage;

public interface ConversationSpotFactory {
    record StartRequest(
        String customerActorId,
        String customerDisplayName,
        String subject,
        long createdAtUnixMs) {
    }

    CompletionStage<Void> start(String conversationId, StartRequest request);
}
