package systems.zlink.samples.kotlin.zoneworld.server.gateway

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSession
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec
class GameSession(
    private val sessionContext: ZLinkSessionContext,
    private val actors: ZLinkActorManager,
) : ZLinkSession {
    override fun context() = sessionContext
    override fun onConnected(): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)
    override fun onDisconnected(): CompletionStage<Void> = CompletableFuture.completedFuture(null)
    override fun onError(error: ZLinkStreamError): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)

    override fun onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): CompletionStage<Void> {
        if (dispatch.packetName() == "JoinWorldReq") return join(dispatch, payload)
        require(sessionContext.actors().bound().size == 1) { "JoinWorldReq must bind an actor first" }
        return sessionContext.actors().bound().single().relay(dispatch, payload)
    }

    private fun join(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): CompletionStage<Void> {
        val request = payload.decode(Messages.JoinWorldReq::class.java)
        return actors.getOrCreate(request.playerId, ZoneWorldNames.PLAYER_ACTOR_TYPE)
            .inMesh(ZoneWorldNames.MESH)
            .request(Messages.EnterWorldReq(ZoneWorldSpec.SPAWN_X, ZoneWorldSpec.SPAWN_Y, false, 0, 0))
            .submit()
            .thenCompose { result ->
                if (result is ZLinkActorCreateResult.Rejected) {
                    return@thenCompose CompletableFuture.failedFuture<Void>(IllegalStateException("actor creation rejected"))
                }
                val actor = when (result) {
                    is ZLinkActorCreateResult.Created -> result.actor()
                    is ZLinkActorCreateResult.Existing -> result.actor()
                    is ZLinkActorCreateResult.Rejected -> error("unreachable")
                }
                sessionContext.actors().bindOrGet(actor)
                    .thenCompose { bound -> bound.relay(dispatch, payload) }
            }
    }
}
