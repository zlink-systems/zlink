using Tutorial.Server.Spots;
using Tutorial.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace Tutorial.Server.Actors;

// A message addressed to a player runs inside the Spot the player currently
// occupies, so handlers receive both. Players in the lobby use the Entry Spot
// interfaces below; players inside a room use IZLinkSpotActor*Handler instead.

// --8<-- [start:actor-handlers]
// --8<-- [start:actor-send-handler]
public sealed class ChangeNicknameHandler
    : IZLinkEntrySpotActorSendHandler<LobbySpot, Player, ChangeNickname>
{
    public async ValueTask HandleAsync(
        LobbySpot lobby,
        Player player,
        IZLinkMessageContext context,
        ChangeNickname message,
        CancellationToken cancellationToken)
    {
        player.Rename(message.Nickname);

        // --8<-- [start:actor-push]
        // Pushes over the connection bound to this player. The same handler also runs
        // on an HTTP path with no bound connection, where push ends with InvalidOperation.
        // Rename is already complete, so only that failure is discarded.
        try
        {
            await player.Context.BoundSession
                .Send(new NicknameChanged(player.Nickname))
                .Async(cancellationToken);
        }
        catch (ZLinkFrameworkException error)
            when (error.Kind == ZLinkFrameworkErrorKind.InvalidOperation)
        {
        }
        // --8<-- [end:actor-push]
    }
}
// --8<-- [end:actor-send-handler]
// --8<-- [start:actor-request-handler]

public sealed class GetPlayerHandler
    : IZLinkEntrySpotActorRequestHandler<LobbySpot, Player, GetPlayer, PlayerInfo>
{
    public ValueTask<PlayerInfo> HandleAsync(
        LobbySpot lobby,
        Player player,
        IZLinkMessageContext context,
        GetPlayer request,
        CancellationToken cancellationToken)
        => ValueTask.FromResult(new PlayerInfo(player.Context.ActorId, player.Nickname));
}
// --8<-- [end:actor-request-handler]
// --8<-- [end:actor-handlers]
