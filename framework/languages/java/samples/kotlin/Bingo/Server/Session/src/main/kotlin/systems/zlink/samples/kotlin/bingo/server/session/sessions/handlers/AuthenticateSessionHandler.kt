package systems.zlink.samples.kotlin.bingo.server.session.sessions.handlers

import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.ZLinkSuspendingTypedSessionPacketHandler
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticatePlayerReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticatePlayerRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticateReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticateRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.EnsurePlayerActorReq

class AuthenticateSessionHandler(
    private val routes: ZLinkRouteClient,
    private val actors: ZLinkActorManager,
) : ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, AuthenticateReq> {
    override fun packetName(): String = "AuthenticateReq"

    override fun messageType(): Class<AuthenticateReq> = AuthenticateReq::class.java

    override suspend fun handle(
        context: ZLinkSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        request: AuthenticateReq,
    ) {
        if (request.accessToken.isBlank()) {
            throw IllegalArgumentException("access token is required")
        }

        val authenticated = routes
            .requestToChannel(
                SampleNames.ApiChannel,
                AuthenticatePlayerReq(request.accessToken),
            )
            .timeout(SampleTimings.RequestTimeout)
            .submit(AuthenticatePlayerRes::class.java)
            .await()
        if (!authenticated.accepted ||
            authenticated.actorId.isBlank() ||
            authenticated.displayName.isBlank()
        ) {
            throw IllegalStateException(
                authenticated.reason ?: "Player authentication failed.",
            )
        }
        val actor = actors.kotlin().getOrCreate(
            authenticated.actorId,
            SampleNames.PlayerActorType,
        )
            .request(EnsurePlayerActorReq(authenticated.actorId, authenticated.displayName))
            .await()
        context.actors().bind(requireActor(actor)).await()
        context.client()
            .reply(
                AuthenticateRes(
                    authenticated.actorId,
                    authenticated.displayName,
                ),
            )
            .submit()
    }

    private fun requireActor(result: ZLinkActorCreateResult): ActorRef = when (result) {
        is ZLinkActorCreateResult.Existing -> result.actor
        is ZLinkActorCreateResult.Created -> result.actor
        is ZLinkActorCreateResult.Rejected -> throw IllegalStateException("Player actor creation was rejected.")
    }
}
