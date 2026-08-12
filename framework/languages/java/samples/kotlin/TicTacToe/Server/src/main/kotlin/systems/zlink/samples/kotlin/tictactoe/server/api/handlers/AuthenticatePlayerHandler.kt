package systems.zlink.samples.kotlin.tictactoe.server.api.handlers

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerInfo

// --8<-- [start:doc-request-handler]
@ZLinkHandlerGroup("api")
class AuthenticatePlayerHandler :
    ZLinkRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes> {
    override fun handle(
        request: AuthenticatePlayerReq,
        context: ZLinkMessageContext,
    ): CompletionStage<AuthenticatePlayerRes> {
        val actorId = request.accessToken.trim()
        require(actorId.isNotBlank()) { "authentication token is empty" }
        return CompletableFuture.completedFuture(
            AuthenticatePlayerRes(
                PlayerInfo(
                    actorId = actorId,
                    displayName = displayName(actorId),
                    level = 3,
                    wins = if (actorId == "player-x") 99 else 0,
                ),
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
