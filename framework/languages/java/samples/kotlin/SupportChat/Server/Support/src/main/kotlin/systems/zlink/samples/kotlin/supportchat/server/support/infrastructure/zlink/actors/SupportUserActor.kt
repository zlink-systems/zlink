package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.configuration.ConversationStatuses
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationFailedNotify
import java.util.concurrent.CompletionStage
import java.util.concurrent.CompletableFuture
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationState

class SupportUserActor(
    val actorId: String,
    private val context: ZLinkActorContext,
) : ZLinkActor {
    var displayName: String = actorId
        private set

    var role: String = ""
        private set

    var participantId: String = actorId
        private set

    var conversationId: String = ""
        private set
    private var pendingConversationId: String? = null
    private val completedJoinOperations = mutableSetOf<systems.zlink.framework.actors.ZLinkActorJoinOperationId>()

    override fun context(): ZLinkActorContext = context

    fun setIdentity(
        displayName: String,
        role: String,
        participantId: String,
    ) {
        this.displayName = displayName
        this.role = role
        this.participantId = participantId
    }

    fun joinConversation(conversationId: String) {
        this.conversationId = conversationId
    }

    fun pendingConversationId(): String = pendingConversationId.orEmpty()

    fun restorePendingConversationJoin(conversationId: String) {
        pendingConversationId = conversationId.ifBlank { null }
    }

    fun completedJoinOperations(): Set<systems.zlink.framework.actors.ZLinkActorJoinOperationId> =
        completedJoinOperations.toSet()

    fun restoreCompletedJoinOperations(
        operationIds: Collection<systems.zlink.framework.actors.ZLinkActorJoinOperationId>,
    ) {
        completedJoinOperations.addAll(operationIds)
    }

    fun scheduleConversationJoin(
        conversationId: String,
        subject: String,
        request: JoinConversationReq,
    ): JoinConversationRes {
        check(pendingConversationId == null) { "A conversation join is already pending" }
        pendingConversationId = conversationId
        context.joinSpot(conversationId, request)
            .timeout(SampleTimings.RequestTimeout)
            .defer()
        return JoinConversationRes(
            scheduled = true,
            state = ConversationState(
                conversationId = conversationId,
                subject = subject,
                status = ConversationStatuses.WaitingForAgent,
                customerActorId = if (request.role == SupportChatRoles.Customer) request.participantId else "",
                agentActorId = null,
                lastMessageSeq = 0,
                lastMessageAtUnixMs = null,
                idleDeadlineUnixMs = null,
            ),
        )
    }

    override fun onJoinCompleted(completion: ZLinkActorJoinCompletion): CompletionStage<Void> {
        val operationId = when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> completion.operationId()
            is ZLinkActorJoinCompletion.Rejected -> completion.operationId()
            is ZLinkActorJoinCompletion.Failed -> completion.operationId()
        }
        if (!completedJoinOperations.add(operationId)) {
            return CompletableFuture.completedFuture(null)
        }
        if (pendingConversationId == null) return CompletableFuture.completedFuture(null)
        val pending = pendingConversationId.orEmpty()
        if (completion is ZLinkActorJoinCompletion.Accepted) {
            conversationId = pendingConversationId.orEmpty()
        }
        pendingConversationId = null
        return when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> CompletableFuture.completedFuture(null)
            is ZLinkActorJoinCompletion.Rejected ->
                context.boundSession()
                    .send(JoinConversationFailedNotify(pending, "Rejected", false))
                    .metadata(SampleNames.ConversationIdMetadataKey, pending)
                    .submit()
            is ZLinkActorJoinCompletion.Failed ->
                context.boundSession()
                    .send(
                        JoinConversationFailedNotify(
                            pending,
                            completion.kind().name,
                            false,
                        ),
                    )
                    .metadata(SampleNames.ConversationIdMetadataKey, pending)
                    .submit()
        }
    }
}
