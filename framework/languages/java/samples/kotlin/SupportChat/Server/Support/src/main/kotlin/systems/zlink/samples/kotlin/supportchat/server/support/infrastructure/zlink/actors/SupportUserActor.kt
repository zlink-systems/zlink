package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors

import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorJoinOperationId
import systems.zlink.framework.kotlin.ZLinkSuspendingActor
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.supportchat.server.configuration.ConversationStatuses
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationState
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationFailedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes

class SupportUserActor(val actorId: String, override val context: ZLinkActorContext) :
    ZLinkSuspendingActor() {
    var displayName: String = actorId
        private set

    var role: String = ""
        private set

    var participantId: String = actorId
        private set

    var conversationId: String = ""
        private set

    private var pendingConversationId: String? = null
    private val completedJoinOperations = mutableSetOf<ZLinkActorJoinOperationId>()

    fun setIdentity(displayName: String, role: String, participantId: String) {
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

    fun completedJoinOperations(): Set<ZLinkActorJoinOperationId> = completedJoinOperations.toSet()

    fun restoreCompletedJoinOperations(operationIds: Collection<ZLinkActorJoinOperationId>) {
        completedJoinOperations.addAll(operationIds)
    }

    fun scheduleConversationJoin(
        conversationId: String,
        subject: String,
        request: JoinConversationReq,
    ): JoinConversationRes {
        check(pendingConversationId == null) { "A conversation join is already pending" }
        pendingConversationId = conversationId
        context.joinSpot(conversationId, request).timeout(SampleTimings.RequestTimeout).defer()
        return JoinConversationRes(
            scheduled = true,
            actorId = actorId,
            state =
                ConversationState(
                    conversationId = conversationId,
                    subject = subject,
                    status = ConversationStatuses.WaitingForAgent,
                    customerActorId =
                        if (request.role == SupportChatRoles.Customer) request.participantId
                        else "",
                    agentActorId = null,
                    lastMessageSeq = 0,
                    lastMessageAtUnixMs = null,
                    idleDeadlineUnixMs = null,
                ),
        )
    }

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        val operationId =
            when (completion) {
                is ZLinkActorJoinCompletion.Accepted -> completion.operationId()
                is ZLinkActorJoinCompletion.Rejected -> completion.operationId()
                is ZLinkActorJoinCompletion.Failed -> completion.operationId()
            }
        if (!completedJoinOperations.add(operationId)) {
            return
        }
        if (pendingConversationId == null) return
        val pending = pendingConversationId.orEmpty()
        if (completion is ZLinkActorJoinCompletion.Accepted) {
            conversationId = pendingConversationId.orEmpty()
        }
        pendingConversationId = null
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> Unit
            is ZLinkActorJoinCompletion.Rejected ->
                context
                    .boundSession()
                    .kotlin()
                    .send(JoinConversationFailedNotify(pending, "Rejected", false))
                    .await()
            is ZLinkActorJoinCompletion.Failed ->
                context
                    .boundSession()
                    .kotlin()
                    .send(JoinConversationFailedNotify(pending, completion.kind().name, false))
                    .await()
        }
    }
}
