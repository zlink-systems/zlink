using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Shared;

public sealed class TownEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<EnterTownHandler, PlayerActor>(nameof(JoinTownRequest));
        Context.Handlers.AddActorPacket<TownEntryTownViewNotification, PlayerActor>(nameof(TownView));
        Context.Handlers.AddActorPacket<TownEntryDungeonViewNotification, PlayerActor>(nameof(DungeonView));
        Context.Handlers.AddActorPacket<TownEntryActionRejectedNotification, PlayerActor>(nameof(ActionRejected));
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

[ZLinkSpotActorSendHandler(nameof(JoinTownRequest))]
public sealed class EnterTownHandler
    : IZLinkEntrySpotActorSendHandler<TownEntry, PlayerActor, JoinTownRequest>
{
    public ValueTask HandleAsync(
        TownEntry entry,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinTownRequest request,
        CancellationToken ct
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity does not match the bound Actor.");
        var reason = ChannelRules.Validate(request.Channel);
        if (reason is not null) return actor.Context.BoundSession.Send(new ActionRejected(reason)).Async();
        actor.Context.JoinSpot(GameNames.TownId(request.Channel), request).Defer();
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorSendHandler(nameof(TownView))]
public sealed class TownEntryTownViewNotification : IZLinkEntrySpotActorSendHandler<TownEntry, PlayerActor, TownView>
{
    public ValueTask HandleAsync(TownEntry spot, PlayerActor actor, IZLinkMessageContext context,
        TownView message, CancellationToken ct) => actor.DeliverAsync(message);
}

[ZLinkSpotActorSendHandler(nameof(DungeonView))]
public sealed class TownEntryDungeonViewNotification : IZLinkEntrySpotActorSendHandler<TownEntry, PlayerActor, DungeonView>
{
    public ValueTask HandleAsync(TownEntry spot, PlayerActor actor, IZLinkMessageContext context,
        DungeonView message, CancellationToken ct) => actor.DeliverAsync(message);
}

[ZLinkSpotActorSendHandler(nameof(ActionRejected))]
public sealed class TownEntryActionRejectedNotification : IZLinkEntrySpotActorSendHandler<TownEntry, PlayerActor, ActionRejected>
{
    public ValueTask HandleAsync(TownEntry spot, PlayerActor actor, IZLinkMessageContext context,
        ActionRejected message, CancellationToken ct) => actor.DeliverAsync(message);
}
