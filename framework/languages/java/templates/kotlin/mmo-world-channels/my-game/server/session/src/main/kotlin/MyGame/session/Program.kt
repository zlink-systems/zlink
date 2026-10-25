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
            .addRouteMesh(Names.mesh(requireNotNull(config.worldId)))
            .listen(requireNotNull(config.meshEndpoint))
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

class GameSession(
    private val session: ZLinkSessionContext,
    private val actors: ZLinkActorManager,
    private val config: NodeConfig,
) : ZLinkSuspendingSession() {
    private val allowedClientPackets =
        setOf(
            EnterWorldRequest::class.java.simpleName,
            ChangeChannelRequest::class.java.simpleName,
            EarnRequest::class.java.simpleName,
            ViewRequest::class.java.simpleName,
            ChannelViewRequest::class.java.simpleName,
            SessionInfoRequest::class.java.simpleName,
        )

    override fun context() = session

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        error("Stream error: $error")
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        if (dispatch.packetName() !in allowedClientPackets) {
            session
                .client()
                .kotlin()
                .send(ActionRejected(Rejection.PACKET_NOT_ALLOWED, 0))
                .await()
            return
        }
        when (dispatch.packetName()) {
            SessionInfoRequest::class.java.simpleName -> {
                val actor = bound()
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
            EnterWorldRequest::class.java.simpleName -> {
                val request = payload.decode<EnterWorldRequest>()
                require(request.playerId.isNotBlank()) { "Player ID is required" }
                val result =
                    actors
                        .kotlin()
                        .getOrCreate(request.playerId, Names.PLAYER)
                        .inMesh(Names.mesh(requireNotNull(config.worldId)))
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
            else -> bound().kotlin().relay(dispatch, payload).await()
        }
    }

    private fun bound(): ZLinkSessionActor {
        val actors = session.actors().bound()
        check(actors.size == 1) { "EnterWorldRequest must be the first packet" }
        return actors.single()
    }
}
