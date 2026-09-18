package systems.zlink.tutorial.server.channel

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.tutorial.shared.GetPlayerProfile
import systems.zlink.tutorial.shared.PlayerProfile

// --8<-- [start:channel-request-handler]
// Answers a request addressed to the "profile" channel. Any node that exposes
// this channel may receive it; the caller does not pick one.
//
// ZLinkSuspendingRequestHandler is the Kotlin surface: the body is a suspend fun
// and returns the reply itself. The Java surface, ZLinkRequestHandler, returns a
// CompletionStage, which a suspend fun cannot override. The group annotation is
// what ties this class to the channel -- see HandlerGroups.kt.
@ZLinkHandlerGroup(HandlerGroups.PROFILE)
class GetPlayerProfileHandler : ZLinkSuspendingRequestHandler<GetPlayerProfile, PlayerProfile> {

    override suspend fun handle(
        request: GetPlayerProfile,
        context: ZLinkMessageContext,
    ): PlayerProfile = PlayerProfile(request.playerId, nickname = "rookie", level = 1)
}
// --8<-- [end:channel-request-handler]
