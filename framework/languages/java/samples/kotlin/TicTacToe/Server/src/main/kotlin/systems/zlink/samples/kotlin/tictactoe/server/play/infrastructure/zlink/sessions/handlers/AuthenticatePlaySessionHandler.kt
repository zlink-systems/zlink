package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.sessions.handlers

import kotlinx.coroutines.future.await
import org.slf4j.LoggerFactory
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
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerActorCreateReq

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
        ).request(PlayerActorCreateReq(authenticated.player)).await()
        val resolvedActor = requireActor(playActor)
        val boundActor = context.actors().bind(requireActor(playActor)).await()
        check(boundActor.ref() == resolvedActor) {
            "Bound ActorRef does not match the resolved ActorRef for '${authenticated.player.actorId}'."
        }
        if (playActor is ZLinkActorCreateResult.Existing) {
            logger.info(
                "play stream: existing actor exact identity verified. " +
                    "sessionId={}, actor={}, generation={}, mesh={}, nodeRid={}",
                context.sessionId(),
                boundActor.actorId(),
                boundActor.ref().objectGeneration(),
                boundActor.ref().meshName(),
                boundActor.ref().nodeRid(),
            )
        }
        context.client()
            .reply(AuthenticateRes(authenticated.player))
            .submit()
            .await()
    }

    private fun requireActor(result: ZLinkActorCreateResult): ActorRef = when (result) {
        is ZLinkActorCreateResult.Existing -> result.actor
        is ZLinkActorCreateResult.Created -> result.actor
        is ZLinkActorCreateResult.Rejected -> throw IllegalStateException("Play actor creation was rejected.")
    }

    private companion object {
        val logger = LoggerFactory.getLogger(AuthenticatePlaySessionHandler::class.java)
    }
}
// --8<-- [end:doc-session-auth]
