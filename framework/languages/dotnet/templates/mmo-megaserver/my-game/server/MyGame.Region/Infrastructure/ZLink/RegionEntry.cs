using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
namespace MyGame.Region;

public sealed class RegionEntry(IZLinkEntrySpotContext context, IZLinkSpotManager spots) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;
    public void Configure()
    {
        Context.Handlers.AddActorPacket<EnterRegion, PlayerActor>(nameof(EnterRegionRequest));
        Context.Handlers.AddActorPacket<NextRegion, PlayerActor>(nameof(RegionFull));
    }
    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
    public async ValueTask SelectAsync(PlayerActor actor, string region, int index, CancellationToken ct)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(region);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(index);
        var id = GameNames.RegionId(region, index);
        var created = await spots.GetOrCreate(id, GameNames.RegionType).InMesh(GameNames.WorldMesh).Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException($"Region creation rejected: {id}.");
        actor.Context.JoinSpot(id, new JoinRegion(region, index)).Defer();
    }
}
[ZLinkSpotActorSendHandler(nameof(EnterRegionRequest))]
public sealed class EnterRegion : IZLinkEntrySpotActorSendHandler<RegionEntry, PlayerActor, EnterRegionRequest>
{
    public ValueTask HandleAsync(RegionEntry entry, PlayerActor actor, IZLinkMessageContext context,
        EnterRegionRequest request, CancellationToken ct)
    {
        if (request.PlayerId != actor.Context.ActorId) throw new InvalidDataException("Player identity mismatch.");
        return entry.SelectAsync(actor, request.Region, 1, ct);
    }
}
[ZLinkSpotActorSendHandler(nameof(RegionFull))]
public sealed class NextRegion : IZLinkEntrySpotActorSendHandler<RegionEntry, PlayerActor, RegionFull>
{
    public ValueTask HandleAsync(RegionEntry entry, PlayerActor actor, IZLinkMessageContext context,
        RegionFull request, CancellationToken ct)
    {
        if (request.Reason != Rejection.RegionFull) throw new InvalidDataException("Unknown region rejection.");
        return entry.SelectAsync(actor, request.Region, checked(request.Index + 1), ct);
    }
}
