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
    private companion object {
        val CLIENT_PACKET_ALLOWLIST =
            setOf(
                QueueRequest::class.java.simpleName,
                RoomViewRequest::class.java.simpleName,
                MoveRequest::class.java.simpleName,
                ShootRequest::class.java.simpleName,
                SessionInfoRequest::class.java.simpleName,
            )
    }

    override fun context() = session

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
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
            QueueRequest::class.java.simpleName -> {
                val request = payload.decode<QueueRequest>()
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
            else ->
                if (dispatch.packetName() in CLIENT_PACKET_ALLOWLIST)
                    session.actors().bound().single().kotlin().relay(dispatch, payload).await()
                else
                    session
                        .client()
                        .kotlin()
                        .send(ActionRejected(Rejection.PACKET_NOT_ALLOWED))
                        .await()
        }
    }
}
