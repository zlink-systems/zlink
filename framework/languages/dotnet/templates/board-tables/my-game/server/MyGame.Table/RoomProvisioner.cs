using MyGame.Shared;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

public sealed class RoomProvisioner(IZLinkInstanceSpotContext context, IZLinkSpotManager spots)
    : IZLinkInstanceSpot
{
    public IZLinkInstanceSpotContext Context { get; } = context;

    public async ValueTask PrepareAsync(PrepareRoom request, CancellationToken ct)
    {
        var room = await spots
            .GetOrCreate(request.AssignmentId, GameNames.TableType)
            .InMesh(GameNames.WorldMesh)
            .Request(request)
            .Async(ct);
        await Context
            .Outbound.SendToSpot(
                MatchNames.PoolKey,
                new RoomPrepared(request.AssignmentId, room.State != ZLinkSpotCreateState.Rejected)
            )
            .InstanceSpot(MatchNames.PoolType)
            .InMesh(GameNames.WorldMesh)
            .Async();
    }

    public async ValueTask WithdrawAsync(WithdrawRoom request, CancellationToken ct)
    {
        // Preparation and withdrawal use the same Instance Spot turn.
        if (await spots.FindAsync(request.AssignmentId, ct) is not null)
            await Context.Outbound.SendToSpot(request.AssignmentId, request).Async();
    }
}
