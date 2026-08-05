package systems.zlink.samples.kotlin.bingo.server.api.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticatePlayerReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticatePlayerRes

@ZLinkHandlerGroup(SampleNames.ApiChannel)
class AuthenticatePlayerHandler(
) : ZLinkSuspendingRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes> {
    override suspend fun handle(
        request: AuthenticatePlayerReq,
        context: ZLinkMessageContext,
    ) = run {
        AuthenticatePlayerRes(
            accepted = true,
            actorId = request.accessToken,
            displayName = request.accessToken,
            reason = null,
        )
    }
}
