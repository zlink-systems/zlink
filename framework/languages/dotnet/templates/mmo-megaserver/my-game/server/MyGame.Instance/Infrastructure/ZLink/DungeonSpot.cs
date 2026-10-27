using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Instance;

public sealed class DungeonSpot(IZLinkSpotContext context, NodeSettings settings)
    : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());

    public void Configure() { }

    public DungeonOpened Info() => new(Context.SpotId, settings.Id);

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

[ZLinkSpotRequestHandler(nameof(DungeonInfoRequest))]
public sealed class DungeonInfo
    : IZLinkSpotRequestHandler<DungeonSpot, DungeonInfoRequest, DungeonOpened>
{
    public ValueTask<DungeonOpened> HandleAsync(
        DungeonSpot spot,
        DungeonInfoRequest request,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Info());
    }
}
