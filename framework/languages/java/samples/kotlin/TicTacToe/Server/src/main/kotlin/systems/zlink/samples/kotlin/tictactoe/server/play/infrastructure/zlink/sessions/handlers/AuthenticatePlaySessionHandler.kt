package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.sessions.handlers

import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.ZLinkSuspendingTypedSessionPacketHandler
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticateReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticateRes

// --8<-- [start:doc-session-auth]
class AuthenticatePlaySessionHandler(
    private val actors: ZLinkActorManager,
    private val channels: ZLinkClient,
) : ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, AuthenticateReq> {
    override fun packetName(): String = "AuthenticateReq"

    override fun messageType(): Class<AuthenticateReq> = AuthenticateReq::class.java

    override suspend fun handle(
        context: ZLinkSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        request: AuthenticateReq,
    ) {
        require(request.accessToken.isNotBlank()) { "access token is required" }
        val authenticated = channels
            .requestToChannel(SampleNames.ApiChannel, AuthenticatePlayerReq(request.accessToken))
            .timeout(SampleNames.RequestTimeout)
            .submit(AuthenticatePlayerRes::class.java)
            .await()
        val playActor = actors.kotlin().getOrCreate(
            authenticated.player.actorId,
            SampleNames.PlayActor,
        ).request(authenticated.player).await()
        context.actors().bind(requireActor(playActor)).await()
        context.client()
            .reply(AuthenticateRes(authenticated.player))
            .submit()
    }

    private fun requireActor(result: ZLinkActorCreateResult): ActorRef = when (result) {
        is ZLinkActorCreateResult.Existing -> result.actor
        is ZLinkActorCreateResult.Created -> result.actor
        is ZLinkActorCreateResult.Rejected -> throw IllegalStateException("Play actor creation was rejected.")
    }
}
// --8<-- [end:doc-session-auth]
