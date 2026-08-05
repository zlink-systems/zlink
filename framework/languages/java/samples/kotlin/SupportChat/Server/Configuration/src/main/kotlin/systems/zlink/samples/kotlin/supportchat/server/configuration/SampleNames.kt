package systems.zlink.samples.kotlin.supportchat.server.configuration

import systems.zlink.samples.kotlin.supportchat.shared.contracts.ChatMessageNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationAssignedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationClosedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationIdleNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ParticipantJoinedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.TypingChangedNotify

object SampleNames {
    const val ApiChannel = "supportchat.api"
    const val SupportChannel = "supportchat.support"
    const val SupportActorType = "supportchat.user"
    const val ConversationSpotType = "supportchat.conversation"
    const val SupportSpotNode = "supportchat.support.node"
    const val SessionSpotNode = "supportchat.session.node"
    const val SupportEntrySpotNode = "supportchat.entry.node"
    const val SupportConversationSpotNode = "supportchat.conversation.node"
    const val SupportSpotDiscovery = "supportchat.support.spots"
    const val StreamNode = "supportchat.stream"
    const val AgentCapacity = 3
    const val ConversationIdMetadataKey = "ConversationId"

    const val ServerEvidenceMarker = "supportchat-server-evidence=completed"
    const val ClientMarker = "supportchat=completed"

    val ParticipantJoinedPacket: String = ParticipantJoinedNotify::class.java.simpleName
    val ConversationAssignedPacket: String = ConversationAssignedNotify::class.java.simpleName
    val ChatMessagePacket: String = ChatMessageNotify::class.java.simpleName
    val TypingChangedPacket: String = TypingChangedNotify::class.java.simpleName
    val ConversationIdlePacket: String = ConversationIdleNotify::class.java.simpleName
    val ConversationClosedPacket: String = ConversationClosedNotify::class.java.simpleName
}

object SupportChatRoles {
    const val Customer = "Customer"
    const val Agent = "Agent"
}

object ConversationStatuses {
    const val WaitingForAgent = "WaitingForAgent"
    const val Active = "Active"
    const val WaitingForClose = "WaitingForClose"
    const val Closed = "Closed"
}
