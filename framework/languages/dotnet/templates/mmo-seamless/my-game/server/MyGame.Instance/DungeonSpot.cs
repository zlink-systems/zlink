using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Instance;

// A game instance is a User Spot. Actor-free Framework Instance Spots use a different activation contract.
public sealed class DungeonSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
}

[ZLinkSpotRequestHandler(nameof(OpenDungeonRequest))]
public sealed class DungeonOpenHandler(NodeSettings settings)
    : IZLinkSpotRequestHandler<DungeonSpot, OpenDungeonRequest, DungeonOpened>
{
    public ValueTask<DungeonOpened> HandleAsync(
        DungeonSpot spot,
        OpenDungeonRequest request,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(new DungeonOpened(spot.Context.SpotId, settings.Id));
}
