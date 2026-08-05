package systems.zlink.samples.kotlin.supportchat.server.support.application

import java.util.concurrent.atomic.AtomicLong

class SupportConversationAllocator(
    private val conversations: ConversationStarter,
) {
    private val gate = Any()
    private val conversationSeq = AtomicLong()

    suspend fun allocate(
        customerActorId: String,
        customerDisplayName: String,
        subject: String,
        createdAtUnixMs: Long = System.currentTimeMillis(),
    ): String {
        val start = synchronized(gate) {
            val conversationId = "supportchat-conversation-${conversationSeq.incrementAndGet()}"
            conversationId to
                ConversationStartReq(
                    customerActorId = customerActorId,
                    customerDisplayName = customerDisplayName,
                    subject = subject,
                    createdAtUnixMs = createdAtUnixMs,
                )
        }
        conversations.start(start.first, start.second)
        return start.first
    }
}

data class ConversationStartReq(
    val customerActorId: String,
    val customerDisplayName: String,
    val subject: String,
    val createdAtUnixMs: Long,
)

interface ConversationStarter {
    suspend fun start(
        conversationId: String,
        request: ConversationStartReq,
    )
}
