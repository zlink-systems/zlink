package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure

import systems.zlink.samples.kotlin.supportchat.server.configuration.ConversationStatuses
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationMessage
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationSnapshot
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationStatus
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ParticipantRole
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ChatMessage
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationState

object ConversationContracts {
    fun toState(snapshot: ConversationSnapshot): ConversationState =
        ConversationState(
            conversationId = snapshot.conversationId,
            subject = snapshot.subject,
            status = toStatus(snapshot.status),
            customerActorId = snapshot.customerActorId,
            agentActorId = snapshot.agentActorId,
            lastMessageSeq = snapshot.lastMessageSeq,
            lastMessageAtUnixMs = snapshot.lastMessageAtUnixMs,
            idleDeadlineUnixMs = snapshot.idleDeadlineUnixMs,
        )

    fun toMessage(message: ConversationMessage): ChatMessage =
        ChatMessage(
            conversationId = message.conversationId,
            messageSeq = message.messageSeq,
            senderActorId = message.senderActorId,
            text = message.text,
            sentAtUnixMs = message.sentAtUnixMs,
        )

    fun toStatus(status: ConversationStatus): String =
        when (status) {
            ConversationStatus.WaitingForAgent -> ConversationStatuses.WaitingForAgent
            ConversationStatus.Active -> ConversationStatuses.Active
            ConversationStatus.WaitingForClose -> ConversationStatuses.WaitingForClose
            ConversationStatus.Closed -> ConversationStatuses.Closed
        }

    fun toRole(role: ParticipantRole): String =
        when (role) {
            ParticipantRole.Customer -> SupportChatRoles.Customer
            ParticipantRole.Agent -> SupportChatRoles.Agent
        }
}
