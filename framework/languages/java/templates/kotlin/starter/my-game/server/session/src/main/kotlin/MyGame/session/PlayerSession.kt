package MyGame.session

import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.*

class PlayerSession(private val session: ZLinkSessionContext) : ZLinkSuspendingSession() {
    override fun context() = session

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        System.err.println("Session error: ${error.error()}: ${error.message()}")
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) = error("No application packets are registered yet.")
}
