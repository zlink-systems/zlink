package systems.zlink.samples.tictactoe.server.api.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticatePlayerReq;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticatePlayerRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerInfo;

// --8<-- [start:doc-request-handler]
@ZLinkHandlerGroup("api")
public final class AuthenticatePlayerHandler
    implements ZLinkRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes> {
    @Override
    public java.util.concurrent.CompletionStage<AuthenticatePlayerRes> handle(
        AuthenticatePlayerReq request,
        ZLinkMessageContext context) {
        String actorId = request.accessToken().trim();
        if (actorId.isBlank()) {
            throw new IllegalArgumentException("authentication token is empty");
        }
        return java.util.concurrent.CompletableFuture.completedFuture(new AuthenticatePlayerRes(new PlayerInfo(
            actorId,
            displayName(actorId),
            3,
            "player-x".equals(actorId) ? 99 : 0)));
    }

    private static String displayName(String actorId) {
        return switch (actorId) {
            case "player-x" -> "Player X";
            case "player-o" -> "Player O";
            default -> "Observer";
        };
    }
}
// --8<-- [end:doc-request-handler]
