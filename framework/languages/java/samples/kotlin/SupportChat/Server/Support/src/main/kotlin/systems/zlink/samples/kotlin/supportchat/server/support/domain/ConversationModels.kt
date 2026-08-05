package systems.zlink.samples.kotlin.supportchat.server.support.domain

import java.time.Duration

enum class ConversationStatus {
    WaitingForAgent,
    Active,
    WaitingForClose,
    Closed,
}

enum class ParticipantRole {
    Customer,
    Agent,
}

data class ConversationPolicy(
    val idleTimeout: Duration,
    val closeGraceTimeout: Duration,
    val maxMessageLength: Int,
) {
    companion object {
        val Sample: ConversationPolicy = ConversationPolicy(
            idleTimeout = Duration.ofSeconds(3),
            closeGraceTimeout = Duration.ofSeconds(2),
            maxMessageLength = 500,
        )
    }
}

data class ConversationParticipant(
    val actorId: String,
    val role: ParticipantRole,
    val displayName: String,
    val joinedAtUnixMs: Long,
    val isTyping: Boolean,
)

data class ConversationMessage(
    val conversationId: String,
    val messageSeq: Long,
    val senderActorId: String,
    val text: String,
    val sentAtUnixMs: Long,
)

data class ConversationSnapshot(
    val conversationId: String,
    val subject: String,
    val status: ConversationStatus,
    val customerActorId: String,
    val agentActorId: String?,
    val lastMessageSeq: Long,
    val lastMessageAtUnixMs: Long?,
    val idleDeadlineUnixMs: Long?,
)

enum class ConversationEventKind {
    ParticipantJoined,
    MessageAppended,
    TypingChanged,
    Idle,
    Closed,
}

data class ConversationEvent(
    val kind: ConversationEventKind,
    val state: ConversationSnapshot,
    val actorId: String? = null,
    val role: ParticipantRole? = null,
    val message: ConversationMessage? = null,
    val isTyping: Boolean? = null,
)

data class ConversationChange(
    val state: ConversationSnapshot,
    val events: List<ConversationEvent>,
)
