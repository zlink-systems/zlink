using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Server.ZoneNode.Application.Zone;
using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots.Handlers;

/// <summary>
/// A human's move request, relayed from the client's session to its actor (§2.1).
/// </summary>
[ZLinkSpotActorSendHandler(nameof(MoveMsg))]
internal sealed class PlayerMoveHandler(
    PlayerMovement movement,
    ILogger<PlayerMoveHandler> logger)
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, MoveMsg>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MoveMsg message,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "player move handler entered. player={PlayerId}, zone={ZoneId}, target=({X},{Y})",
            actor.ActorId,
            spot.ZoneId,
            message.X,
            message.Y);
        return movement.MoveAsync(spot, actor, message.X, message.Y, cancellationToken);
    }
}

/// <summary>
/// One bot step (§2.7). A bot is the same actor type as a human and walks the same
/// movement path — validation, zone change and relocation are identical. The only
/// difference is that a rejection turns it around instead of being pushed to a client.
/// </summary>
[ZLinkSpotActorSendHandler(nameof(BotTickMsg))]
internal sealed class PlayerBotTickHandler(PlayerMovement movement)
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, BotTickMsg>
{
    public async ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        BotTickMsg message,
        CancellationToken cancellationToken)
    {
        _ = context;
        _ = message;
        if (!actor.IsBot) return;

        var target = BotPatrolPolicy.NextStep(actor.Position, actor.DirX, actor.DirY);
        await movement.MoveAsync(spot, actor, target.X, target.Y, cancellationToken);
    }
}

/// <summary>
/// Receives a zone snapshot on the Actor's serialized turn and forwards it through
/// the session currently bound to that Actor.
/// </summary>
[ZLinkSpotActorSendHandler(nameof(DeliverZoneStateMsg))]
internal sealed class PlayerZoneStateDeliveryHandler(ILogger<PlayerZoneStateDeliveryHandler> logger)
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, DeliverZoneStateMsg>
{
    public async ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        DeliverZoneStateMsg message,
        CancellationToken cancellationToken)
    {
        if (actor.IsBot) return;
        var ownView = message.Players.FirstOrDefault(player =>
            string.Equals(player.PlayerId, actor.ActorId, StringComparison.Ordinal));
        logger.LogInformation(
            "zone state delivery handler entered. player={PlayerId}, zone={ZoneId}, "
            + "tick={Tick}, players={PlayerCount}, own_view={OwnViewPlayerId}@({OwnViewX},{OwnViewY})/"
            + "{OwnViewZoneId}",
            actor.ActorId,
            message.ZoneId,
            message.Tick,
            message.Players.Count,
            ownView?.PlayerId ?? "<missing>",
            ownView?.X,
            ownView?.Y,
            ownView?.ZoneId ?? "<missing>");
        await actor.Context.BoundSession
            .Send(new ZoneStateNotify(message.ZoneId, message.Tick, message.Players))
            .Async(cancellationToken);
        logger.LogInformation(
            "zone state delivery handler completed. player={PlayerId}, zone={ZoneId}",
            actor.ActorId,
            message.ZoneId);
    }
}

/// <summary>
/// Reports a committed zone change through the Actor's current bound session.
/// </summary>
[ZLinkSpotActorSendHandler(nameof(DeliverZoneChangedMsg))]
internal sealed class PlayerZoneChangedDeliveryHandler
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, DeliverZoneChangedMsg>
{
    public async ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        DeliverZoneChangedMsg message,
        CancellationToken cancellationToken)
    {
        if (actor.IsBot) return;
        await actor.Context.BoundSession
            .Send(new ZoneChangedNotify(message.PlayerId, message.ZoneId))
            .Async(cancellationToken);
    }
}

/// <summary>
/// Receives a world announcement on the Actor's serialized turn and forwards it
/// through the session currently bound to that Actor.
/// </summary>
[ZLinkSpotActorSendHandler(nameof(DeliverWorldAnnounceMsg))]
internal sealed class PlayerWorldAnnouncementDeliveryHandler
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, DeliverWorldAnnounceMsg>
{
    public async ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        DeliverWorldAnnounceMsg message,
        CancellationToken cancellationToken)
    {
        if (actor.IsBot) return;
        await actor.Context.BoundSession
            .Send(new WorldAnnounceNotify(message.AnnouncementId, message.Text))
            .Async(cancellationToken);
    }
}

/// <summary>
/// Returns the probe payload unchanged. The runner reaches this handler through the
/// public Actor client after it has deliberately retained the previous-owner route.
/// </summary>
[ZLinkSpotActorRequestHandler(nameof(MessageFollowProbeReq))]
internal sealed class PlayerMessageFollowProbeHandler(
    ILogger<PlayerMessageFollowProbeHandler> logger)
    : IZLinkSpotActorRequestHandler<
        ZoneSpot,
        PlayerActor,
        MessageFollowProbeReq,
        MessageFollowProbeRes>
{
    public ValueTask<MessageFollowProbeRes> HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MessageFollowProbeReq request,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "message-follow probe handled. actor={ActorId}, probe={ProbeId}, payload={Payload}",
            actor.ActorId,
            request.ProbeId,
            Convert.ToHexString(request.Payload));
        return ValueTask.FromResult(new MessageFollowProbeRes(request.ProbeId, request.Payload));
    }

/// <summary>
/// Handles the one-way half of the runner-only Message Follow probe. The application records
/// the operation because a one-way send has no reply that could prove target execution.
/// </summary>
[ZLinkSpotActorSendHandler(nameof(MessageFollowProbeReq))]
internal sealed class PlayerMessageFollowProbeSendHandler(
    ILogger<PlayerMessageFollowProbeSendHandler> logger)
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, MessageFollowProbeReq>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MessageFollowProbeReq message,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "message-follow probe one-way handled. actor={ActorId}, probe={ProbeId}, payload={Payload}",
            actor.ActorId,
            message.ProbeId,
            Convert.ToHexString(message.Payload));
        return ValueTask.CompletedTask;
    }
}
}

/// <summary>
/// The one movement path both humans and bots take (§2.1, §2.7). It lives here rather
/// than in the Application layer because a zone change is a JoinSpot, and JoinSpot is a
/// ZLink call that must run inside the actor's own handler turn.
/// </summary>
internal sealed class PlayerMovement(
    MoveUseCase moves,
    IZLinkSpotClient spots,
    ILogger<PlayerMovement> logger)
{
    public async ValueTask MoveAsync(
        ZoneSpot spot,
        PlayerActor actor,
        int toX,
        int toY,
        CancellationToken cancellationToken)
    {
        switch (moves.Decide(actor.Position, toX, toY))
        {
            case MoveDecision.Rejected rejected:
                await RejectAsync(actor, rejected.Reason, cancellationToken);
                return;

            case MoveDecision.Accepted { ZoneChanged: false } stayed:
                actor.MoveTo(stayed.To);
                await spots
                    .SendToSpot(
                        spot.ZoneId,
                        new UpdatePositionMsg(
                            actor.ActorId,
                            stayed.To.X,
                            stayed.To.Y,
                            actor.IsBot))
                    .Async(cancellationToken);
                return;

            case MoveDecision.Accepted accepted:
                await ChangeZoneAsync(actor, accepted.To, cancellationToken);
                return;
        }
    }

    /// <summary>
    /// Joining the target zone's spot is what moves the player, and when that spot lives
    /// on another node the join causes relocation (§2.6). The target spot is the authority
    /// on its own maintenance state, so it may still refuse — in which case the departing
    /// node's cache was stale and the coordinate stays where it was (§2.3).
    /// </summary>
    private ValueTask ChangeZoneAsync(
        PlayerActor actor,
        PlayerPosition to,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        actor.TrackDeferredJoin(to);
        actor.Context
            .JoinSpot(
                to.ZoneId,
                new EnterZoneMsg(
                    actor.ActorId,
                    to.X,
                    to.Y,
                    actor.IsBot,
                    InitialEntry: false,
                    FromNodeId: ZoneTopology.NodeOf(actor.ZoneId)))
            .Defer();

        logger.LogInformation(
            "zone change scheduled. player={PlayerId}, zone={ZoneId}",
            actor.ActorId,
            to.ZoneId);
        return ValueTask.CompletedTask;
    }

    /// <summary>
    /// A refused move leaves the coordinate untouched (§2.2). A human is told why; a bot
    /// has no client to tell, so it turns around and walks back (§2.7).
    /// </summary>
    private async ValueTask RejectAsync(
        PlayerActor actor,
        string reason,
        CancellationToken cancellationToken)
    {
        if (actor.IsBot)
        {
            actor.ReverseDirection();
            return;
        }

        await actor.Context.BoundSession
            .Send(new MoveRejectedNotify(reason, actor.Position.X, actor.Position.Y))
            .Async(cancellationToken);
    }
}
