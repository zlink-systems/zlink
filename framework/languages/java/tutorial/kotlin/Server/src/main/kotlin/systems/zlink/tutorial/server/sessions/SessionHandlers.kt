package systems.zlink.tutorial.server.sessions

import java.time.Duration
import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.kotlin.ZLinkSuspendingTypedSessionPacketHandler
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.tutorial.shared.Authenticate
import systems.zlink.tutorial.shared.Authenticated
import systems.zlink.tutorial.shared.CreatePlayer
import systems.zlink.tutorial.shared.Ping
import systems.zlink.tutorial.shared.Pong

// --8<-- [start:session-handler]
// The first type argument is the session context, not the session class. A
// suspending session handler names its packet itself, which the Java surface
// takes from messageType() alone.
class PingHandler : ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, Ping> {

    override fun packetName(): String = "Ping"

    override fun messageType(): Class<Ping> = Ping::class.java

    override suspend fun handle(
        context: ZLinkSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        message: Ping,
    ) {
        // reply answers a request. To push to a client that is not waiting for
        // one, use client().send instead.
        context.client().reply(Pong(message.sentAtUnixMs)).submit().await()
    }
}

// --8<-- [end:session-handler]

// --8<-- [start:session-actor-bind]
// Ties this connection to one player. After this, packets without a session
// handler reach that player, and the player can push to this connection.
class AuthenticateHandler(private val players: ZLinkActorManager) :
    ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, Authenticate> {

    override fun packetName(): String = "Authenticate"

    override fun messageType(): Class<Authenticate> = Authenticate::class.java

    override suspend fun handle(
        context: ZLinkSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        message: Authenticate,
    ) {
        // A returning client finds its existing player rather than a new one.
        val result =
            players
                .getOrCreate(message.playerId, "player")
                .inMesh("game")
                .request(CreatePlayer(message.playerId))
                .timeout(Duration.ofSeconds(10))
                .submit()
                .await()

        val bound = context.actors().bindOrGet(resolve(result)).await()

        context.client().reply(Authenticated(bound.actorId())).submit().await()
    }

    private fun resolve(result: ZLinkActorCreateResult): ActorRef =
        when (result) {
            is ZLinkActorCreateResult.Existing -> result.actor()
            is ZLinkActorCreateResult.Created -> result.actor()
            else -> throw IllegalStateException("Player creation was rejected.")
        }
}
// --8<-- [end:session-actor-bind]
