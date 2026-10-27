using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Shared;

public sealed class WorldEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<EnterWorldHandler, PlayerActor>(nameof(JoinWorldRequest));
        Context.Handlers.AddActorPacket<WorldEntryPartyViewNotification, PlayerActor>(nameof(PartyView));
        Context.Handlers.AddActorPacket<WorldEntryBorderVisibleNotification, PlayerActor>(nameof(BorderVisible));
        Context.Handlers.AddActorPacket<WorldEntryDungeonViewNotification, PlayerActor>(nameof(DungeonView));

    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

[ZLinkSpotActorSendHandler(nameof(JoinWorldRequest))]
public sealed class EnterWorldHandler
    : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, JoinWorldRequest>
{
    public ValueTask HandleAsync(
        WorldEntry entry,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinWorldRequest request,
        CancellationToken cancellationToken
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity does not match the bound Actor.");
        actor.Move(new EnterField(request.X, 0));
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorSendHandler(nameof(PartyView))]
public sealed class WorldEntryPartyViewNotification : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, PartyView>
{
    public ValueTask HandleAsync(WorldEntry spot, PlayerActor actor, IZLinkMessageContext context,
        PartyView message, CancellationToken ct) => actor.DeliverAsync(message);
}

[ZLinkSpotActorSendHandler(nameof(BorderVisible))]
public sealed class WorldEntryBorderVisibleNotification : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, BorderVisible>
{
    public ValueTask HandleAsync(WorldEntry spot, PlayerActor actor, IZLinkMessageContext context,
        BorderVisible message, CancellationToken ct) => actor.DeliverAsync(message);
}

[ZLinkSpotActorSendHandler(nameof(DungeonView))]
public sealed class WorldEntryDungeonViewNotification : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, DungeonView>
{
    public ValueTask HandleAsync(WorldEntry spot, PlayerActor actor, IZLinkMessageContext context,
        DungeonView message, CancellationToken ct) => actor.DeliverAsync(message);
}
