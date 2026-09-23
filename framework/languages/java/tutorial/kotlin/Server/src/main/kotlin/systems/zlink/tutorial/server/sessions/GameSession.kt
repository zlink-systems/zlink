package systems.zlink.tutorial.server.sessions

import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError

// --8<-- [start:session-class]
// One connected game client. Callbacks for the same connection run in order.
// The dispatcher is handed in by the Framework and holds the handlers the
// stream node registered.
class GameSession(
    private val context: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
) : ZLinkSuspendingSession() {

    override fun context(): ZLinkSessionContext = context

    override suspend fun onConnectedSuspending() {
        println("client connected: ${context.sessionId()}")
    }

    override suspend fun onDisconnectedSuspending() {
        println("client disconnected: ${context.sessionId()}")
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        println("stream error on ${context.sessionId()}: $error")
    }

    // Every inbound packet arrives here first. Registering a handler is not
    // enough on its own; this method is what routes the packet to it.
    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        if (handlers.tryHandle(context, dispatch, payload).await()) return

        // --8<-- [start:session-actor-relay]
        // A slotted packet uses its dispatch Actor. An unslotted packet can use
        // the connection only while exactly one Actor is bound.
        val player =
            dispatch.actor()
                ?: run {
                    val bound = context.actors().bound()
                    when (bound.size) {
                        1 -> bound[0]
                        0 -> error("Authenticate an Actor before sending player packets.")
                        else -> error("Select an Actor handle when more than one Actor is bound.")
                    }
                }

        player.kotlin().relay(dispatch, payload).await()
        // --8<-- [end:session-actor-relay]
    }
}
// --8<-- [end:session-class]
