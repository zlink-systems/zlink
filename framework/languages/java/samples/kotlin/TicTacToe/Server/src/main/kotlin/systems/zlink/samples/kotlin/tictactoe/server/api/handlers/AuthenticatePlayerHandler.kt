package systems.zlink.samples.kotlin.tictactoe.server.api.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerInfo

// --8<-- [start:doc-request-handler]
@ZLinkHandlerGroup("api")
class AuthenticatePlayerHandler :
    ZLinkSuspendingRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes> {
    override suspend fun handle(
        request: AuthenticatePlayerReq,
        context: ZLinkMessageContext,
    ): AuthenticatePlayerRes {
        val actorId = request.accessToken.trim()
        require(actorId.isNotBlank()) { "authentication token is empty" }
        return AuthenticatePlayerRes(
            PlayerInfo(
                actorId = actorId,
                displayName = displayName(actorId),
                level = 3,
                wins = if (actorId == "player-x") 99 else 0,
            ),
        )
    }

    private fun displayName(actorId: String): String =
        when (actorId) {
            "player-x" -> "Player X"
            "player-o" -> "Player O"
            else -> "Observer"
        }
}
// --8<-- [end:doc-request-handler]
