package systems.zlink.samples.kotlin.bingo.server.session.sessions

import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkSessionDispatchContext

class BingoSession(
    private val context: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = context

    override suspend fun onDisconnectedSuspending() {
        for (actor in context.actors().bound()) {
            actor.notifyDisconnected().await()
        }
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        val handled = handlers.tryHandle(context, dispatch, payload).await()
        if (handled) {
            return
        }
        val actor = requireSingleBoundActor(dispatch.packetName())
        actor.relay(dispatch, payload).await()
    }

    private fun requireSingleBoundActor(packetName: String): ZLinkSessionActor =
        when (context.actors().bound().size) {
            1 -> context.actors().bound()[0]
            0 -> throw IllegalStateException(
                "Client must authenticate before relaying packet '$packetName'",
            )
            else -> throw IllegalStateException(
                "Exactly one actor must be bound before relaying packet '$packetName'",
            )
        }
}
