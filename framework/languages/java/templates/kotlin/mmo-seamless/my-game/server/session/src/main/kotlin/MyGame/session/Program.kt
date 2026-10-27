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

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        val packetName = dispatch.packetName()
        val allowed =
            setOf(
                JoinWorldRequest::class.java.simpleName,
                MoveRequest::class.java.simpleName,
                OpenDungeonRequest::class.java.simpleName,
                SessionInfoRequest::class.java.simpleName,
            )
        if (packetName !in allowed) {
            session.client().kotlin().send(ActionRejected("packet-not-allowed")).await()
            return
        }
        when (packetName) {
            SessionInfoRequest::class.java.simpleName -> {
                val actor = session.actors().bound().single()
                val generation = actor.ref().objectGeneration()
                if (generation > Rules.MAX_SAFE_INTEGER) {
                    session.client().kotlin().send(ActionRejected("integer-out-of-range")).await()
                    return
                }
                session
                    .client()
                    .kotlin()
                    .reply(
                        SessionInfo(
                            session.sessionId(),
                            actor.actorId(),
                            generation,
                        )
                    )
                    .await()
            }
            JoinWorldRequest::class.java.simpleName -> {
                if (session.actors().bound().isNotEmpty()) {
                    session.actors().bound().single().kotlin().relay(dispatch, payload).await()
                    return
                }
                val request = payload.decode<JoinWorldRequest>()
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
