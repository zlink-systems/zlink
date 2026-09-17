using Tutorial.Server.Spots;
using Tutorial.Shared;
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
        // Reaches the connection bound to this player. If none is bound, the
        // call does nothing rather than failing.
        await player.Context.BoundSession
            .Send(new NicknameChanged(player.Nickname))
            .Async(cancellationToken);
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
