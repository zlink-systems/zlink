package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.framework.messaging.ZLinkMessage
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError

class ScenarioSession(
    private val context: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
    private val evidence: ScenarioState
) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = context

    override suspend fun onConnectedSuspending() {
        evidence.record("StreamConnected", "session", context.sessionId())
    }

    override suspend fun onDisconnectedSuspending() {
        evidence.record("StreamDisconnected", "session", context.sessionId())
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        evidence.record("StreamError", "session", error.error().name)
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage
    ) {
        evidence.record("StreamInbound", "session", dispatch.packetName())
        val handled = handlers.tryHandle(context, dispatch, payload).await()
        if (handled) {
            return
        }
        requireActor(dispatch).relay(dispatch, payload).await()
    }

    private fun requireActor(dispatch: ZLinkSessionDispatchContext): ZLinkSessionActor {
        val actorId = dispatch.metadata()["actor-id"]
        if (actorId != null && actorId.isNotBlank()) {
            return context.actors().find(actorId)
                .orElseThrow { IllegalStateException("actor is not bound: $actorId") }
        }
        return when (context.actors().bound().size) {
            1 -> context.actors().bound()[0]
            0 -> throw IllegalStateException("ActorAuthReq is required before actor packet")
            else -> throw IllegalStateException("actor-id metadata is required for multiple bound actors")
        }
    }
}
