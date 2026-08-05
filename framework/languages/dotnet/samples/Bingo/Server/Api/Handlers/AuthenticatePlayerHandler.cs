using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;

namespace Bingo.Server.Api.Handlers;

[ZLinkHandlerGroup("api")]
internal sealed class AuthenticatePlayerHandler
    : IZLinkRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes>
{
    public ValueTask<AuthenticatePlayerRes> HandleAsync(
        AuthenticatePlayerReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        if (!request.AccessToken.StartsWith("player-", StringComparison.Ordinal)
            && !string.Equals(request.AccessToken, BingoSamplePlayers.Observer, StringComparison.Ordinal))
            return ValueTask.FromResult(new AuthenticatePlayerRes
            {
                Accepted = false,
                Reason = "Access token must be a sample player id."
            });

        var displayName = string.Equals(request.AccessToken, BingoSamplePlayers.Observer, StringComparison.Ordinal)
            ? "Observer"
            : request.AccessToken.Replace("player-", "Player ", StringComparison.Ordinal);
        return ValueTask.FromResult(new AuthenticatePlayerRes
        {
            Accepted = true,
            ActorId = request.AccessToken,
            DisplayName = displayName
        });
    }
}
