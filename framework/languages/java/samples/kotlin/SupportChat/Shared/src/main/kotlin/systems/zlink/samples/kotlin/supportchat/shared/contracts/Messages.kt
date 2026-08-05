package systems.zlink.samples.kotlin.supportchat.shared.contracts

import systems.zlink.framework.actors.ActorRefSnapshot

data class AuthenticateReq(
    val accessToken: String,
)

data class AuthenticateRes(
    val actorId: String,
    val displayName: String,
    val role: String,
)

data class AuthenticateUserReq(
    val accessToken: String,
)

data class AuthenticateUserRes(
    val accepted: Boolean,
    val actorId: String?,
    val displayName: String?,
    val role: String?,
    val reason: String?,
)

data class OpenConversationApiReq(
    val customerActorId: String,
    val customerDisplayName: String,
    val subject: String,
)

data class OpenConversationApiRes(
    val conversationId: String,
    val status: String,
)

data class AllocateConversationReq(
    val customerActorId: String,
    val customerDisplayName: String,
    val subject: String,
)

data class AllocateConversationRes(
    val conversationId: String,
    val status: String,
    val state: ConversationState? = null,
)

data class EnsureSupportUserActorReq(
    val actorId: String,
    val displayName: String,
    val role: String,
    val participantId: String = actorId,
)

data class EnsureSupportUserActorRes(
    val actor: ActorRefSnapshot,
)

data class EnsureAgentConversationReq(
    val rosterActorId: String,
    val displayName: String,
    val conversationId: String,
)

data class EnsureAgentConversationRes(
    val actor: ActorRefSnapshot,
    val scheduled: Boolean,
    val state: ConversationState,
)

data class OpenConversationReq(
    val subject: String,
)

data class OpenConversationRes(
    val conversationId: String,
    val state: ConversationState,
)

data class SetAgentAvailableReq(
    val isAvailable: Boolean,
)

data class SetAgentAvailableRes(
    val isAvailable: Boolean,
)

data class JoinConversationReq(
    val participantId: String = "",
    val role: String = "",
    val displayName: String = "",
)

data class JoinConversationRes(
    val scheduled: Boolean,
    val state: ConversationState,
)

data class JoinConversationFailedNotify(
    val conversationId: String,
    val error: String,
    val isRetriable: Boolean,
)

data class SendChatMessageReq(
    val text: String,
)

data class SendChatMessageRes(
    val message: ChatMessage,
    val state: ConversationState,
)

data class SetTypingReq(
    val isTyping: Boolean,
)

data class CloseConversationReq(
    val reason: String?,
)

data class CloseConversationRes(
    val state: ConversationState,
)

data class ParticipantJoinedNotify(
    val conversationId: String,
    val actorId: String,
    val role: String,
    val state: ConversationState,
)

data class ConversationAssignedNotify(
    val conversationId: String,
    val state: ConversationState,
)

data class ChatMessageNotify(
    val conversationId: String,
    val message: ChatMessage,
    val state: ConversationState,
)

data class TypingChangedNotify(
    val conversationId: String,
    val actorId: String,
    val isTyping: Boolean,
    val state: ConversationState,
)

data class ConversationIdleNotify(
    val conversationId: String,
    val state: ConversationState,
)

data class ConversationClosedNotify(
    val conversationId: String,
    val state: ConversationState,
)

data class ConversationState(
    val conversationId: String,
    val subject: String,
    val status: String,
    val customerActorId: String,
    val agentActorId: String?,
    val lastMessageSeq: Long,
    val lastMessageAtUnixMs: Long?,
    val idleDeadlineUnixMs: Long?,
)

data class ChatMessage(
    val conversationId: String,
    val messageSeq: Long,
    val senderActorId: String,
    val text: String,
    val sentAtUnixMs: Long,
)
