using MyGame.Lobby.Application;
using MyGame.Shared;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Lobby.Infrastructure.ZLink;

public sealed class RoomProvisioner(IZLinkSpotManager spots, NodeSettings settings)
    : IRoomProvisioner
{
    public async Task<MatchResponse> PrepareAsync(string roomId, CancellationToken ct)
    {
        var created = await spots
            .GetOrCreate(roomId, GameNames.RoomType)
            .InMesh(GameNames.WorldMesh)
            .Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Room creation was rejected.");
        return new MatchResponse(
            created.Spot.SpotId,
            settings.SessionEndpoint
                ?? throw new InvalidOperationException("SessionEndpoint is required.")
        );
    }
}
