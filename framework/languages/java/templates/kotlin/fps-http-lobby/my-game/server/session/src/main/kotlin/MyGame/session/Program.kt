package MyGame.session

import MyGame.*
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.context.annotation.Bean
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.locations.redis.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spring.*
import systems.zlink.framework.streams.*

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class Program {
    @Bean
    fun framework(
        config: NodeConfig,
        locations: ZLinkRedisLocationStore,
        relocations: ZLinkRedisRelocationStore,
    ) = ZLinkFrameworkConfigurer { options ->
        configure(options, locations, relocations)
        options
            .addRouteMesh(Names.MESH)
            .listen(config.meshEndpoint)
            .setRoutingIdPrefix(config.id)
            .objects()
            .client()
        options
            .addStreamNode(Names.STREAM)
            .bind(requireNotNull(config.streamEndpoint))
            .enableActorDispatch()
            .registerSession<GameSession>()
    }
}

fun main(args: Array<String>) = runNode(Program::class.java, args)

class GameSession(private val session: ZLinkSessionContext, private val actors: ZLinkActorManager) :
    ZLinkSuspendingSession() {
    override fun context() = session

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        error("Stream error: $error")
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        val packet = dispatch.packetName()
        val bound = session.actors().bound().singleOrNull()
        if (bound == null) {
            if (packet != JoinRoomRequest::class.java.simpleName) {
                rejectPacket()
                return
            }
            val request = payload.decode<JoinRoomRequest>()
            require(request.playerId.isNotBlank() && request.roomId.isNotBlank())
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
                    is ZLinkActorCreateResult.Rejected -> error("Actor creation rejected: $result")
                }
            session.actors().bindOrGet(actor).await().kotlin().relay(dispatch, payload).await()
            return
        }
        when (packet) {
            SessionInfoRequest::class.java.simpleName ->
                session
                    .client()
                    .kotlin()
                    .reply(
                        SessionInfo(
                            session.sessionId(),
                            bound.actorId(),
                            bound.ref().objectGeneration(),
                        )
                    )
                    .await()
            RoomViewRequest::class.java.simpleName,
            MoveRequest::class.java.simpleName,
            ShootRequest::class.java.simpleName ->
                bound.kotlin().relay(dispatch, payload).await()
            else -> rejectPacket()
        }
    }

    private suspend fun rejectPacket() {
        session.client().kotlin().send(ActionRejected(Rejection.PACKET_NOT_ALLOWED)).await()
    }
}
