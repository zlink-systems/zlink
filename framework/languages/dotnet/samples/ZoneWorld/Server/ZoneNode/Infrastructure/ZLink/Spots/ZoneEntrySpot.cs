using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;
using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots;

/// <summary>
/// Where a player actor is born. An actor created through the actor manager lands here,
/// and it stays only long enough to join its zone spot — which is the step that actually
/// puts it in the world (§2.6).
/// </summary>
public sealed class ZoneEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult<ZLinkSpotActorJoinResult>(ZLinkSpotActorJoinResult.Accept());

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

/// <summary>
/// Puts a freshly created actor into the world. The join has to run inside the actor's
/// own handler turn, so the ensure path asks the actor to do it rather than doing it
/// from the outside.
/// </summary>
[ZLinkSpotActorRequestHandler(nameof(EnterWorldReq))]
internal sealed class PlayerEnterWorldHandler(ILogger<PlayerEnterWorldHandler> logger)
    : IZLinkEntrySpotActorRequestHandler<ZoneEntrySpot, PlayerActor, EnterWorldReq, EnterWorldRes>
{
    public async ValueTask<EnterWorldRes> HandleAsync(
        ZoneEntrySpot entrySpot,
        PlayerActor actor,
        IZLinkMessageContext context,
        EnterWorldReq message,
        CancellationToken cancellationToken)
    {
        return await EnterWorld.RunAsync(actor, message, logger, cancellationToken);
    }
}

/// <summary>
/// A human's entry, relayed from its session (§7.1). It is handled by the actor rather than
/// by the Gateway so that the relay itself reaches the node hosting the actor — that relay is
/// what tells the node where to push.
/// </summary>
[ZLinkSpotActorSendHandler(nameof(JoinWorldReq))]
internal sealed class PlayerJoinWorldHandler(ILogger<PlayerJoinWorldHandler> logger)
    : IZLinkEntrySpotActorSendHandler<ZoneEntrySpot, PlayerActor, JoinWorldReq>
{
    public async ValueTask HandleAsync(
        ZoneEntrySpot entrySpot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinWorldReq message,
        CancellationToken cancellationToken)
    {
        _ = entrySpot;
        _ = context;
        await EnterWorld.RunAsync(
            actor,
            new EnterWorldReq(ZoneWorldSpec.SpawnX, ZoneWorldSpec.SpawnY, IsBot: false),
            logger,
            cancellationToken);
    }
}

internal static class EnterWorld
{
    public static ValueTask<EnterWorldRes> RunAsync(
        PlayerActor actor,
        EnterWorldReq message,
        ILogger logger,
        CancellationToken cancellationToken)
    {
        // The patrol direction has to be on the actor before it joins, because the zone
        // spot preserves it across the join and relocation carries it to the next node.
        actor.SetPatrol(message.DirX, message.DirY);
        // The join is deferred. Record the bot identity before scheduling it so a rejected
        // admission cannot send a human-only notification to a bot with no session.
        actor.PrepareEntry(message.IsBot);

        var zoneId = ZoneWorldSpec.ZoneOf(message.X, message.Y);
        actor.TrackDeferredJoin(
            new PlayerPosition(message.X, message.Y),
            message.IsBot
                ? PlayerJoinPurpose.InitialBotEntry
                : PlayerJoinPurpose.InitialHumanEntry);
        actor.Context.JoinSpot(
            zoneId,
            new EnterZoneReq(
                actor.ActorId,
                message.X,
                message.Y,
                message.IsBot,
                InitialEntry: true,
                FromZoneId: null))
            .Defer();

        logger.LogInformation(
            "player entry scheduled. player={PlayerId}, zone={ZoneId}, bot={IsBot}",
            actor.ActorId,
            zoneId,
            message.IsBot);

        return ValueTask.FromResult(
            new EnterWorldRes(zoneId, message.X, message.Y));
    }
}
