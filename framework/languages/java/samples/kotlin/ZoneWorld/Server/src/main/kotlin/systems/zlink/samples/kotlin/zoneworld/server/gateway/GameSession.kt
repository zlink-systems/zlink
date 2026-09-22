package systems.zlink.samples.kotlin.zoneworld.server.gateway

import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToActor
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames

class GameSession(
    private val sessionContext: ZLinkSessionContext,
    private val actors: ZLinkActorManager,
    private val actorClient: ZLinkActorClient,
    private val probes: RelocationProbeService,
) : ZLinkSuspendingSession() {
    private val kotlinActors = actors.kotlin()
    private val kotlinActorClient = actorClient.kotlin()

    override fun context() = sessionContext

    override suspend fun onConnectedSuspending() {}

    override suspend fun onDisconnectedSuspending() {}

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {}

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        if (dispatch.packetName() == "RelocationPairReq") {
            sessionContext.client().kotlin().reply(probes.selectPair().await()).await()
            return
        }
        if (dispatch.packetName() == "ActorLocationProbeReq") {
            val request = payload.decode<Messages.ActorLocationProbeReq>()
            sessionContext
                .client()
                .kotlin()
                .reply(probes.findActor(request.actorId).await())
                .await()
            return
        }
        if (dispatch.packetName() == "FreshActorProbeReq") {
            val request = payload.decode<Messages.FreshActorProbeReq>()
            sessionContext.client().kotlin().reply(probes.createFresh(request.actorId)).await()
            return
        }
        if (dispatch.packetName() == "MessageFollowProbeReq") {
            val request = payload.decode<Messages.MessageFollowProbeReq>()
            val response =
                kotlinActorClient
                    .requestToActor<Messages.MessageFollowProbeRes>(request.actorId, request)
                    .await()
            sessionContext.client().kotlin().reply(response).await()
            return
        }
        if (dispatch.packetName() == "MessageFollowProbeMsg") {
            val message = payload.decode<Messages.MessageFollowProbeMsg>()
            kotlinActorClient.sendToActor(message.actorId, message).await()
            return
        }
        if (dispatch.packetName() == "JoinWorldMsg") {
            join(dispatch, payload)
            return
        }
        require(sessionContext.actors().bound().size == 1) {
            "JoinWorldMsg must bind an actor first"
        }
        sessionContext.actors().bound().single().relay(dispatch, payload).await()
    }

    private suspend fun join(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        val request = payload.decode<Messages.JoinWorldMsg>()
        // --8<-- [start:doc-zw-session-bind]
        val result =
            kotlinActors
                .getOrCreate(request.playerId, ZoneWorldNames.PLAYER_ACTOR_TYPE)
                .inMesh(ZoneWorldNames.MESH)
                .request(ZLinkMessage.empty())
                .await()
        if (result is ZLinkActorCreateResult.Rejected) error("actor creation rejected")
        val actor =
            when (result) {
                is ZLinkActorCreateResult.Created -> result.actor
                is ZLinkActorCreateResult.Existing -> result.actor
                is ZLinkActorCreateResult.Rejected -> error("unreachable")
            }
        sessionContext.actors().bindOrGet(actor).await().relay(dispatch, payload).await()
        // --8<-- [end:doc-zw-session-bind]
    }
}
