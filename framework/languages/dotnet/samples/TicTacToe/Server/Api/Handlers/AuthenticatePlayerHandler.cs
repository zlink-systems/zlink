using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;

namespace TicTacToe.Server.Api.Handlers;

// --8<-- [start:doc-request-handler]
[ZLinkHandlerGroup("api")]
internal sealed class AuthenticatePlayerHandler(ILogger<AuthenticatePlayerHandler> logger)
    : IZLinkRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes>
{
    public ValueTask<AuthenticatePlayerRes> HandleAsync(
        AuthenticatePlayerReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {

        var actorId = request.AccessToken.Trim();
        if (string.IsNullOrWhiteSpace(actorId)) throw new InvalidOperationException("Authentication token is empty.");

        var player = CreatePlayer(actorId);

        logger.LogInformation(
            "play -> api: authenticate accepted. player={ActorId}, level={Level}, wins={Wins}",
            player.ActorId,
            player.Level,
            player.Wins);
        return ValueTask.FromResult(new AuthenticatePlayerRes(player));
    }

    private static PlayerInfo CreatePlayer(string actorId)
    {
        return actorId switch
        {
            "player-x" => new PlayerInfo(actorId, "Player X", 5, 99),
            "player-o" => new PlayerInfo(actorId, "Player O", 4, 12),
            "observer" => new PlayerInfo(actorId, "Observer", 1, 0),
            _ => new PlayerInfo(actorId, actorId, 3, 0)
        };
    }
}
// --8<-- [end:doc-request-handler]
