package systems.zlink.samples.kotlin.supportchat.server.support.domain

class Conversation(
    val conversationId: String,
    val subject: String,
    val customerActorId: String,
    customerDisplayName: String,
    createdAtUnixMs: Long,
    private val policy: ConversationPolicy = ConversationPolicy.Sample,
) {
    private val messages = mutableListOf<ConversationMessage>()
    private val participants = linkedMapOf<String, ConversationParticipant>()
    private var closeDeadlineUnixMs: Long? = null
    private var idleDeadlineUnixMs: Long? = null
    private var lastMessageAtUnixMs: Long? = null
    private var lastMessageSeq: Long = 0

    var status: ConversationStatus = ConversationStatus.WaitingForAgent
        private set

    var agentActorId: String? = null
        private set

    init {
        require(subject.isNotBlank()) { "Conversation subject is required." }
        participants[customerActorId] = ConversationParticipant(
            actorId = customerActorId,
            role = ParticipantRole.Customer,
            displayName = customerDisplayName,
            joinedAtUnixMs = createdAtUnixMs,
            isTyping = false,
        )
    }

    fun snapshot(): ConversationSnapshot =
        ConversationSnapshot(
            conversationId = conversationId,
            subject = subject,
            status = status,
            customerActorId = customerActorId,
            agentActorId = agentActorId,
            lastMessageSeq = lastMessageSeq,
            lastMessageAtUnixMs = lastMessageAtUnixMs,
            idleDeadlineUnixMs = idleDeadlineUnixMs,
        )

    fun joinAgent(
        agentActorId: String,
        agentDisplayName: String,
        joinedAtUnixMs: Long,
    ): ConversationChange {
        ensureNotClosed("join an agent")
        val currentAgent = this.agentActorId
        check(currentAgent == null || currentAgent == agentActorId) {
            "Conversation already has an assigned agent."
        }

        this.agentActorId = agentActorId
        status = ConversationStatus.Active
        participants[agentActorId] = ConversationParticipant(
            actorId = agentActorId,
            role = ParticipantRole.Agent,
            displayName = agentDisplayName,
            joinedAtUnixMs = joinedAtUnixMs,
            isTyping = false,
        )

        val state = snapshot()
        return ConversationChange(
            state = state,
            events = listOf(
                ConversationEvent(
                    kind = ConversationEventKind.ParticipantJoined,
                    state = state,
                    actorId = agentActorId,
                    role = ParticipantRole.Agent,
                ),
            ),
        )
    }

    fun sendMessage(
        senderActorId: String,
        text: String,
        sentAtUnixMs: Long,
    ): ConversationChange {
        ensureParticipant(senderActorId)
        check(status != ConversationStatus.Closed) { "Closed conversation cannot accept messages." }
        check(status != ConversationStatus.WaitingForAgent) { "Conversation is waiting for an agent." }
        require(text.isNotBlank()) { "Message text is required." }
        require(text.length <= policy.maxMessageLength) { "Message text is too long." }

        status = ConversationStatus.Active
        lastMessageSeq += 1
        lastMessageAtUnixMs = sentAtUnixMs
        idleDeadlineUnixMs = sentAtUnixMs + policy.idleTimeout.toMillis()
        closeDeadlineUnixMs = null
        val message = ConversationMessage(
            conversationId = conversationId,
            messageSeq = lastMessageSeq,
            senderActorId = senderActorId,
            text = text,
            sentAtUnixMs = sentAtUnixMs,
        )
        messages += message

        val state = snapshot()
        return ConversationChange(
            state = state,
            events = listOf(
                ConversationEvent(
                    kind = ConversationEventKind.MessageAppended,
                    state = state,
                    actorId = senderActorId,
                    message = message,
                ),
            ),
        )
    }

    fun setTyping(actorId: String, isTyping: Boolean): ConversationChange {
        val participant = participants[actorId]
        if (status == ConversationStatus.Closed || participant == null) {
            return ConversationChange(snapshot(), emptyList())
        }

        participants[actorId] = participant.copy(isTyping = isTyping)
        val state = snapshot()
        return ConversationChange(
            state = state,
            events = listOf(
                ConversationEvent(
                    kind = ConversationEventKind.TypingChanged,
                    state = state,
                    actorId = actorId,
                    role = participant.role,
                    isTyping = isTyping,
                ),
            ),
        )
    }

    fun markIdle(nowUnixMs: Long): ConversationChange {
        val idleDeadline = idleDeadlineUnixMs
        if (status == ConversationStatus.Active && idleDeadline != null && nowUnixMs >= idleDeadline) {
            status = ConversationStatus.WaitingForClose
            closeDeadlineUnixMs = nowUnixMs + policy.closeGraceTimeout.toMillis()
            val state = snapshot()
            return ConversationChange(
                state = state,
                events = listOf(ConversationEvent(ConversationEventKind.Idle, state)),
            )
        }

        val closeDeadline = closeDeadlineUnixMs
        if (status == ConversationStatus.WaitingForClose && closeDeadline != null && nowUnixMs >= closeDeadline) {
            status = ConversationStatus.Closed
            val state = snapshot()
            return ConversationChange(
                state = state,
                events = listOf(ConversationEvent(ConversationEventKind.Closed, state)),
            )
        }

        return ConversationChange(snapshot(), emptyList())
    }

    fun close(actorId: String, reason: String?): ConversationChange {
        reason?.let { _ -> }
        ensureParticipant(actorId)
        check(status != ConversationStatus.Closed) { "Conversation is already closed." }

        status = ConversationStatus.Closed
        val state = snapshot()
        return ConversationChange(
            state = state,
            events = listOf(
                ConversationEvent(
                    kind = ConversationEventKind.Closed,
                    state = state,
                    actorId = actorId,
                ),
            ),
        )
    }

    private fun ensureParticipant(actorId: String): ConversationParticipant =
        participants[actorId] ?: error("Actor is not a conversation participant.")

    private fun ensureNotClosed(action: String) {
        check(status != ConversationStatus.Closed) { "Closed conversation cannot $action." }
    }
}
