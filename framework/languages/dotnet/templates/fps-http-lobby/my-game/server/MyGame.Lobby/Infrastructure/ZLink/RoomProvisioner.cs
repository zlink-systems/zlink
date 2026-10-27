using MyGame.Lobby.Application;
using MyGame.Shared;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Lobby.Infrastructure.ZLink;

public sealed class RoomProvisioner(IZLinkSpotManager spots, IZLinkSpotClient client, NodeSettings settings)
    : IRoomProvisioner
{
    public async Task<MatchResponse> PrepareAsync(string roomId, string[] members, CancellationToken ct)
    {
        var created = await spots.GetOrCreate(roomId, GameNames.RoomType)
            .InMesh(GameNames.WorldMesh).Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Room creation was rejected.");
        var prepared = await client.RequestToSpot(created.Spot.SpotId, new PrepareRoom(roomId, members))
            .Async<RoomPrepared>(ct);
        if (prepared.MatchId != roomId || created.Spot.SpotId != roomId)
            throw new InvalidDataException("Prepared Room identity differs from the match.");
        return new MatchResponse(roomId, settings.SessionEndpoint
            ?? throw new InvalidOperationException("SessionEndpoint is required."));
    }
}
