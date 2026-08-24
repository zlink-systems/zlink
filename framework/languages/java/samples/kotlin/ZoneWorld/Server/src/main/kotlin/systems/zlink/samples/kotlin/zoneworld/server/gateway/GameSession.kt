package systems.zlink.samples.kotlin.zoneworld.server.gateway

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.actors.ZLinkActorClient
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
    private val actorClient: ZLinkActorClient,
    private val probes: RelocationProbeService,
) : ZLinkSession {
    override fun context() = sessionContext
    override fun onConnected(): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)
    override fun onDisconnected(): CompletionStage<Void> = CompletableFuture.completedFuture(null)
    override fun onError(error: ZLinkStreamError): CompletionStage<Void> = CompletableFuture.completedFuture<Void>(null)

    override fun onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): CompletionStage<Void> {
        if (dispatch.packetName() == "RelocationPairReq") return probes.selectPair()
            .thenCompose { sessionContext.client().reply(it).submit() }
        if (dispatch.packetName() == "ActorLocationProbeReq") {
            val request = payload.decode(Messages.ActorLocationProbeReq::class.java)
            return probes.findActor(request.actorId).thenCompose { sessionContext.client().reply(it).submit() }
        }
        if (dispatch.packetName() == "FreshActorProbeReq") {
            val request = payload.decode(Messages.FreshActorProbeReq::class.java)
            return probes.createFresh(request.actorId).thenCompose { sessionContext.client().reply(it).submit() }
        }
        if (dispatch.packetName() == "MessageFollowProbeReq") {
            val request = payload.decode(Messages.MessageFollowProbeReq::class.java)
            return actorClient.requestToActor(request.actorId, request)
                .submit(Messages.MessageFollowProbeRes::class.java)
                .thenCompose { sessionContext.client().reply(it).submit() }
        }
        if (dispatch.packetName() == "MessageFollowProbeMsg") {
            val message = payload.decode(Messages.MessageFollowProbeMsg::class.java)
            return actorClient.sendToActor(message.actorId, message).submit()
        }
        if (dispatch.packetName() == "JoinWorldMsg") return join(dispatch, payload)
        require(sessionContext.actors().bound().size == 1) { "JoinWorldMsg must bind an actor first" }
        return sessionContext.actors().bound().single().relay(dispatch, payload)
    }

    private fun join(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): CompletionStage<Void> {
        val request = payload.decode(Messages.JoinWorldMsg::class.java)
        return actors.getOrCreate(request.playerId, ZoneWorldNames.PLAYER_ACTOR_TYPE)
            .inMesh(ZoneWorldNames.MESH)
            .request(ZLinkMessage.empty())
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
