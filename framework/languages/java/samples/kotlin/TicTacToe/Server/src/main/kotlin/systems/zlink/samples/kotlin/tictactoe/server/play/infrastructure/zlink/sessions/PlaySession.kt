package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.sessions

import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkSessionDispatchContext

// --8<-- [start:doc-session]
class PlaySession(
    private val context: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = context

    override suspend fun onDisconnectedSuspending() {
        context.actors().bound().forEach { actor ->
            actor.notifyDisconnected().await()
        }
    }

    override suspend fun onDispatchSuspending(header: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        if (handlers.tryHandle(context, header, payload).await()) {
            return
        }
        requireActor(header.packetName()).relay(header, payload).await()
    }

    private fun requireActor(packetName: String): ZLinkSessionActor =
        when (context.actors().bound().size) {
            1 -> context.actors().bound()[0]
            0 -> throw IllegalStateException("AuthenticateReq is required before play packet '$packetName'")
            else -> throw IllegalStateException("Exactly one actor must be bound before play packet '$packetName'")
        }
}
// --8<-- [end:doc-session]
