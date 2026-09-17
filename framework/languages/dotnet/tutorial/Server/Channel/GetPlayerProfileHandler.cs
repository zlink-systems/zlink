using Tutorial.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace Tutorial.Server.Channel;

// --8<-- [start:channel-request-handler]
// Answers a request addressed to the "profile" channel. Any node that exposes
// this channel may receive it; the caller does not pick one.
public sealed class GetPlayerProfileHandler
    : IZLinkRequestHandler<GetPlayerProfile, PlayerProfile>
{
    public ValueTask<PlayerProfile> HandleAsync(
        GetPlayerProfile request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
        => ValueTask.FromResult(new PlayerProfile(request.PlayerId, "rookie", Level: 1));
}
// --8<-- [end:channel-request-handler]
