package systems.zlink.samples.supportchat.server.support.actors;
import java.io.IOException;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;

import com.fasterxml.jackson.databind.ObjectMapper;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

public final class SupportUserActorRelocationAdapter
    implements ZLinkActorRelocationAdapter<SupportUserActor> {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Override
    public CompletionStage<byte[]> capture(
        SupportUserActor actor,
        ZLinkRelocationCancellation cancellation) {
        try {
            return CompletableFuture.completedFuture(
                JSON.writeValueAsBytes(new TransferState(
                    actor.displayName(),
                    actor.role(),
                    actor.participantId(),
                    actor.conversationId(),
                    actor.pendingConversationId(),
                    actor.completedJoinOperations())));
        } catch (IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    @Override
    public CompletionStage<Void> restore(
        SupportUserActor actor,
        byte[] state,
        ZLinkRelocationCancellation cancellation) {
        try {
            TransferState transferred = JSON.readValue(state, TransferState.class);
            actor.setIdentity(
                transferred.displayName(),
                transferred.role(),
                transferred.participantId());
            actor.joinConversation(transferred.conversationId());
            actor.restorePendingConversationJoin(
                transferred.pendingConversationId());
            actor.restoreCompletedJoinOperations(
                transferred.completedJoinOperations());
            return CompletableFuture.completedFuture(null);
        } catch (IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public record TransferState(
        String displayName,
        String role,
        String participantId,
        String conversationId,
        String pendingConversationId,
        Set<ZLinkActorJoinOperationId>
            completedJoinOperations) {
    }
}
