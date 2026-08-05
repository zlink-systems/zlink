package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter
import systems.zlink.framework.actors.ZLinkRelocationCancellation

class SupportUserActorRelocationAdapter : ZLinkActorRelocationAdapter<SupportUserActor> {
    override fun capture(
        actor: SupportUserActor,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> = CompletableFuture.completedFuture(
        json.writeValueAsBytes(
            TransferState(
                actor.displayName,
                actor.role,
                actor.participantId,
                actor.conversationId,
                actor.pendingConversationId(),
                actor.completedJoinOperations(),
            ),
        ),
    )

    override fun restore(
        actor: SupportUserActor,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        val transferred = json.readValue(state, TransferState::class.java)
        actor.setIdentity(transferred.displayName, transferred.role, transferred.participantId)
        if (transferred.conversationId.isNotBlank()) {
            actor.joinConversation(transferred.conversationId)
        }
        actor.restorePendingConversationJoin(transferred.pendingConversationId)
        actor.restoreCompletedJoinOperations(transferred.completedJoinOperations)
        return CompletableFuture.completedFuture(null)
    }

    companion object {
        private val json = jacksonObjectMapper()
    }

    data class TransferState(
        val displayName: String,
        val role: String,
        val participantId: String,
        val conversationId: String,
        val pendingConversationId: String,
        val completedJoinOperations:
            Set<systems.zlink.framework.actors.ZLinkActorJoinOperationId>,
    )
}
