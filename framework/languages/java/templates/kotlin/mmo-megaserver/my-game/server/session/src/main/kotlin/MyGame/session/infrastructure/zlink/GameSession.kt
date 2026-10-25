package MyGame.session.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.*

class GameSession(private val session: ZLinkSessionContext, private val actors: ZLinkActorManager) :
    ZLinkSuspendingSession() {
    private val allowedPackets = setOf("EnterRegionRequest", "MoveRequest", "ViewRequest", "OpenDungeonRequest", "SessionInfoRequest")
    override fun context() = session

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        if (dispatch.packetName() !in allowedPackets) {
            session.client().kotlin().send(ActionRejected("packet-not-allowed", 0)).await()
            return
        }
        when (dispatch.packetName()) {
            SessionInfoRequest::class.java.simpleName -> {
                val actor = session.actors().bound().single()
                session
                    .client()
                    .kotlin()
                    .reply(
                        SessionInfo(
                            session.sessionId(),
                            actor.actorId(),
                            actor.ref().objectGeneration(),
                        )
                    )
                    .await()
            }
            EnterRegionRequest::class.java.simpleName -> {
                val request = payload.decode<EnterRegionRequest>()
                require(request.playerId.isNotBlank())
                val result =
                    actors
                        .kotlin()
                        .getOrCreate(request.playerId, Names.PLAYER)
                        .inMesh(Names.MESH)
                        .request(ZLinkMessage.empty())
                        .await()
                val actor =
                    when (result) {
                        is ZLinkActorCreateResult.Created -> result.actor
                        is ZLinkActorCreateResult.Existing -> result.actor
                        is ZLinkActorCreateResult.Rejected ->
                            error("Actor creation rejected: $result")
                    }
                session.actors().bindOrGet(actor).await().kotlin().relay(dispatch, payload).await()
            }
            else -> session.actors().bound().single().kotlin().relay(dispatch, payload).await()
        }
    }
}
