package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.sessions

import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError

class CustomerSession(
    private val sessionContext: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = sessionContext

    override suspend fun onConnectedSuspending() {
    }

    override suspend fun onDisconnectedSuspending() {
        for (actor in sessionContext.actors().bound()) actor.notifyDisconnected().await()
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
    }

    override suspend fun onDispatchSuspending(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        val handled = handlers.tryHandle(sessionContext, dispatch, payload).await()
        if (handled) {
            return
        }
        val actor = when (sessionContext.actors().bound().size) {
            1 -> sessionContext.actors().bound()[0]
            0 -> error("Client must subscribe before relaying packet '${dispatch.packetName()}'")
            else -> error("Exactly one customer actor must be bound before relaying packet '${dispatch.packetName()}'")
        }
        actor.relay(dispatch, payload).await()
    }
}
