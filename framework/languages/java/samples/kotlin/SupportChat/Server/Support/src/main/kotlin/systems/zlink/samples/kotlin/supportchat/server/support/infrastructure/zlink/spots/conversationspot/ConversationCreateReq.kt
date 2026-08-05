package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot

data class ConversationCreateReq(
    val customerActorId: String,
    val customerDisplayName: String,
    val subject: String,
    val createdAtUnixMs: Long,
)
