package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot

import java.time.Duration
import org.slf4j.LoggerFactory
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotClosingContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.support.application.AgentAssignmentService
import systems.zlink.samples.kotlin.supportchat.server.support.domain.Conversation
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationChange
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationEventKind
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationPolicy
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.ConversationContracts
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportActorDirectory
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.handlers.ConversationIdleTimerHandler
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.notifications.ConversationNotificationPublisher
import systems.zlink.samples.kotlin.supportchat.shared.contracts.CloseConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.CloseConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SendChatMessageReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SendChatMessageRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetTypingReq

class ConversationSpot(
    override val context: ZLinkSpotContext,
    private val notifications: ConversationNotificationPublisher,
    private val assignment: AgentAssignmentService,
    private val directory: SupportActorDirectory,
) : ZLinkSuspendingSpot<SupportUserActor>() {
    private val actors = linkedMapOf<String, SupportUserActor>()
    private val pendingJoins = mutableSetOf<String>()
    private var conversation: Conversation? = null
    private var idleTimer: ZLinkTimer? = null

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        val create = request.decode(ConversationCreateReq::class.java)
        val conversationId = context.spotId()
        conversation = Conversation(
            conversationId = conversationId,
            subject = create.subject,
            customerActorId = create.customerActorId,
            customerDisplayName = create.customerDisplayName,
            createdAtUnixMs = create.createdAtUnixMs,
            policy = ConversationPolicy(
                SampleTimings.IdleTimeout,
                SampleTimings.CloseGraceTimeout,
                500,
            ),
        )
        logger.info(
            "support conversation: created. conversation={}, customer={}",
            conversationId,
            create.customerActorId,
        )
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onInitializeSuspending() {
        idleTimer = context.addTimer(
            "conversation-idle",
            Duration.ofMillis(200),
            ConversationIdleTimerHandler::class.java,
            null,
        ).await()
    }

    override suspend fun onClosingSuspending(context: ZLinkSpotClosingContext) {
        idleTimer?.cancel()?.await()
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val conversation = requireConversation()
        request.decode(JoinConversationReq::class.java)
        pendingJoins += actorId
        return ZLinkSpotActorJoinResult.accept(
            JoinConversationRes(
                scheduled = false,
                state = ConversationContracts.toState(conversation.snapshot()),
            ),
        )
    }

    override suspend fun onJoinedActorSuspending(actor: SupportUserActor) {
        check(pendingJoins.remove(actor.actorId)) {
            "joined actor does not have a pending admission"
        }
        val conversation = requireConversation()
        if (actor.role == SupportChatRoles.Agent) {
            val change = joinAgent(actor)
            publishChange(change)
            return
        }

        actor.joinConversation(conversation.conversationId)
        actors[actor.participantId] = actor
        assignAgent()
        logger.info(
            "support conversation: actor joined. conversation={}, participant={}, role={}",
            conversation.conversationId,
            actor.participantId,
            actor.role,
        )
    }

    override suspend fun onLeaveActorSuspending(actor: SupportUserActor) {
        pendingJoins.remove(actor.actorId)
        actors.remove(actor.participantId)
    }

    suspend fun checkIdle() {
        val change = conversation?.markIdle(System.currentTimeMillis()) ?: return
        publishChange(change)
    }

    fun refreshMembership(actor: SupportUserActor): JoinConversationRes {
        val conversation = requireConversation()
        actors[actor.participantId] = actor
        logger.info(
            "support conversation: membership refreshed. conversation={}, participant={}",
            conversation.conversationId,
            actor.participantId,
        )
        return JoinConversationRes(
            scheduled = false,
            state = ConversationContracts.toState(conversation.snapshot()),
        )
    }

    suspend fun sendMessage(actor: SupportUserActor, request: SendChatMessageReq): SendChatMessageRes {
        val change = requireConversation().sendMessage(actor.participantId, request.text, System.currentTimeMillis())
        publishChange(change)
        val message = change.events.single { it.kind == ConversationEventKind.MessageAppended }.message
            ?: error("Message event was not created.")
        return SendChatMessageRes(
            ConversationContracts.toMessage(message),
            ConversationContracts.toState(change.state),
        )
    }

    suspend fun setTyping(actor: SupportUserActor, request: SetTypingReq) {
        publishChange(requireConversation().setTyping(actor.participantId, request.isTyping))
    }

    suspend fun close(actor: SupportUserActor, request: CloseConversationReq): CloseConversationRes {
        val change = requireConversation().close(actor.participantId, request.reason)
        publishChange(change)
        return CloseConversationRes(ConversationContracts.toState(change.state))
    }

    private suspend fun assignAgent() {
        val conversation = requireConversation()
        val assigned = assignment.assignForConversation(conversation.conversationId) ?: return
        notifications.publishAssignedToRoster(
            directory.get(assigned.rosterActorId).actor,
            conversation.snapshot(),
        )
        logger.info(
            "support conversation: assigned. conversation={}, roster={}",
            conversation.conversationId,
            assigned.rosterActorId,
        )
    }

    private fun joinAgent(agent: SupportUserActor): ConversationChange {
        val conversation = requireConversation()
        val change = conversation.joinAgent(agent.participantId, agent.displayName, System.currentTimeMillis())
        agent.joinConversation(conversation.conversationId)
        actors[agent.participantId] = directory.get(agent.participantId).actor
        return change
    }

    private suspend fun publishChange(change: ConversationChange) {
        notifications.publish(change.events, actors)
        for (event in change.events) {
            logger.info(
                "support conversation: state changed. conversation={}, status={}, event={}",
                event.state.conversationId,
                event.state.status,
                event.kind,
            )
        }
        if (change.events.any { it.kind == ConversationEventKind.Closed }) {
            assignment.releaseConversation(requireConversation().conversationId)
        }
    }

    private fun requireConversation(): Conversation =
        conversation ?: error("Conversation has not been created.")

    private companion object {
        private val logger = LoggerFactory.getLogger(ConversationSpot::class.java)
    }
}
