using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Server.ZoneNode.Application.Zone;
using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots.Handlers;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots;

/// <summary>
/// One geographic zone. Its SpotId is the ZoneId, so entering a zone means joining the
/// spot named after it — and when that spot lives on another node, the join is what
/// relocates the actor (§2.6). Nothing else moves a player between zones.
///
/// The spot keeps a copy of each resident's position for rendering and border sync. The
/// authority is the player actor (§2.1); this copy never overrules it.
/// </summary>
public sealed class ZoneSpot(
    IZLinkSpotContext context,
    NodeMaintenancePolicy maintenance,
    NodePlayerCensus census,
    IZLinkActorClient actors,
    ILogger<ZoneSpot> logger) : IZLinkSpot<PlayerActor>
{
    private readonly ZoneState _state = new(context.SpotId);
    private readonly Dictionary<string, EnterZoneMsg> _pendingJoins = new(StringComparer.Ordinal);
    private readonly HashSet<string> _observedBorderSources = new(StringComparer.Ordinal);
    private IZLinkTimer? _tick;
    private IZLinkTimer? _botTick;

    public IZLinkSpotContext Context { get; } = context;

    public string ZoneId => _state.ZoneId;

    public string NodeId => maintenance.OwnNodeId;

    public void Configure()
    {
        // Startup validation has no concrete SpotId. Registering one representative topic
        // lets it validate the handler type; each real spot then registers only its own
        // incoming edge topics.
        if (string.IsNullOrEmpty(Context.SpotId))
        {
            Context.Handlers.AddSubscribe<ZoneBorderSubscriptionHandler>(
                ZoneWorldNames.ZoneChannel,
                ZoneWorldNames.NorthWestToNorthEastBorder);
            return;
        }

        foreach (var fromZoneId in World.AdjacentZones(ZoneId))
            Context.Handlers.AddSubscribe<ZoneBorderSubscriptionHandler>(
                ZoneWorldNames.ZoneChannel,
                ZoneWorldNames.BorderTopic(fromZoneId, ZoneId));
    }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        _tick = await Context.AddTimer<ZoneTickHandler>(
            $"zone-tick-{ZoneId}",
            TimeSpan.FromMilliseconds(ZoneWorldSpec.TickPeriodMs),
            cancellationToken: cancellationToken);
        _botTick = await Context.AddTimer<BotTickHandler>(
            $"bot-tick-{ZoneId}",
            TimeSpan.FromMilliseconds(ZoneWorldSpec.BotTickPeriodMs),
            cancellationToken: cancellationToken);
        logger.LogInformation("zone spot ready. zone={ZoneId}, node={NodeId}", ZoneId, NodeId);
    }

    public async ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        if (_tick is not null) await _tick.CancelAsync();
        if (_botTick is not null) await _botTick.CancelAsync();
    }

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());

    /// <summary>
    /// Admission (§2.3). This node is the authority on its own maintenance state, so a
    /// stale cache on the node the player is leaving cannot let anyone in. Maintenance
    /// blocks arrivals from another node and blocks new entries into the world; it does
    /// not block a player who is already on this node from moving between its zones.
    /// The framework does not hand the source node to this callback, so the join payload
    /// carries it.
    /// </summary>
    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var enter = request.Decode<EnterZoneMsg>();
        if (maintenance.RejectsArrival(maintenance.OwnNodeId, enter.FromNodeId))
        {
            logger.LogInformation(
                "zone spot: join rejected, node under maintenance. zone={ZoneId}, player={PlayerId}, from_node={FromNodeId}",
                ZoneId,
                enter.PlayerId,
                enter.FromNodeId ?? "<new>");
            return ValueTask.FromResult<ZLinkSpotActorJoinResult>(
                ZLinkSpotActorJoinResult.Reject(
                    new EnterZoneRes(ZoneId, MoveRejectReasons.ZoneMaintenance)));
        }

        _pendingJoins[actorId] = enter;
        return ValueTask.FromResult<ZLinkSpotActorJoinResult>(
            ZLinkSpotActorJoinResult.Accept(new EnterZoneRes(ZoneId)));
    }

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        if (!_pendingJoins.Remove(actor.ActorId, out var enter)) return;

        actor.Restore(enter.X, enter.Y, ZoneId, enter.IsBot, actor.DirX, actor.DirY);
        _state.Enter(enter.PlayerId, enter.X, enter.Y, enter.IsBot);
        census.Record(ZoneId, _state.PlayerCount);

        // A brand-new entry learns its zone from JoinWorldRes, so only a zone *change*
        // is announced here. For a remote relocation the framework invokes this callback only
        // after the handoff commits, making the notification a safe boundary for the client's
        // next command.
        if (!enter.IsBot && !enter.InitialEntry)
            await actors
                .SendToActor(
                    actor.ActorId,
                    new DeliverZoneChangedMsg(
                        enter.PlayerId,
                        ZoneId))
                .Async(cancellationToken);

        logger.LogInformation(
            "zone spot: player entered. zone={ZoneId}, player={PlayerId}, bot={IsBot}, initial={InitialEntry}",
            ZoneId,
            enter.PlayerId,
            enter.IsBot,
            enter.InitialEntry);
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        // The player id is the actor id: the ensure path creates the actor under the
        // player's own id, so the two never diverge.
        Remove(actor.ActorId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        // A client that goes away leaves the world. Keeping the player in the zone would mean
        // pushing a tick at a session that no longer exists, every 100ms, for as long as the
        // node runs — and it would leave the player visible to everyone else as a body that
        // never moves.
        Remove(actor.ActorId);
        logger.LogInformation(
            "zone spot: player left, client gone. zone={ZoneId}, player={PlayerId}",
            ZoneId,
            actor.ActorId);
        return ValueTask.CompletedTask;
    }

    internal void Remove(string playerId)
    {
        _state.Leave(playerId);
        census.Record(ZoneId, _state.PlayerCount);
    }

    internal void ApplyPositionUpdate(UpdatePositionMsg message) =>
        _state.UpdatePosition(message.PlayerId, message.X, message.Y, message.IsBot);

    /// <summary>
    /// Restores a disconnected human to this zone's projection after a new session binds to
    /// the same actor. The actor keeps the authoritative coordinate while disconnected; this
    /// aggregate restores only the resident copy used for ticks and border synchronization.
    /// </summary>
    internal JoinWorldRes Rejoin(PlayerActor actor)
    {
        var position = actor.Position;
        _state.Enter(actor.ActorId, position.X, position.Y, actor.IsBot);
        census.Record(ZoneId, _state.PlayerCount);

        return new JoinWorldRes(
            actor.ActorId,
            ZoneId,
            position.X,
            position.Y,
            null);
    }

    internal void ApplyBorderSnapshot(ZoneBorderEvent snapshot)
    {
        _state.ApplyBorderSnapshot(snapshot.FromZoneId, snapshot.Tick, snapshot.Players);
        if (_observedBorderSources.Add(snapshot.FromZoneId))
            logger.LogInformation(
                "border subscription ready. zone={ZoneId}, from={FromZoneId}",
                ZoneId,
                snapshot.FromZoneId);
    }

    internal async ValueTask TickAsync(CancellationToken cancellationToken)
    {
        var output = ZoneTickUseCase.Advance(_state);

        await PushToClientsAsync(
            output.PushTargets,
            new DeliverZoneStateMsg(
                output.Notify.ZoneId,
                output.Notify.Tick,
                output.Notify.Players),
            cancellationToken);

        foreach (var borderEvent in output.BorderEvents)
        {
            await Context.Outbound
                .Publish(
                    ZoneWorldNames.ZoneChannel,
                    ZoneWorldNames.BorderTopic(ZoneId, borderEvent.ToZoneId),
                    borderEvent)
                .Async(cancellationToken);
        }

    }

    /// <summary>
    /// Drives this zone's bots (§2.7). A bot moves down the same path a human does, and
    /// that path can end in a JoinSpot, which only an actor handler turn may start — so
    /// the bot is driven by a request addressed to it rather than by calling into it here.
    /// The timer submits one step to each actor and then releases the Spot turn. The actor jobs
    /// remain serialized and cannot deadlock by waiting for their owning SpotWide turn.
    /// </summary>
    internal async ValueTask BotTickAsync(CancellationToken cancellationToken)
    {
        foreach (var playerId in ZoneTickUseCase.Bots(_state))
        {
            await actors
                .SendToActor(playerId, new BotTickMsg())
                .Async(cancellationToken);
        }
    }

    /// <summary>Delivers an announcement to every human in this zone (§8.2).</summary>
    internal async ValueTask DeliverAnnounceAsync(
        DeliverAnnounceMsg announce,
        CancellationToken cancellationToken)
    {
        // The canonical asks that *every* zone spot receive the announcement (§11 ZW-D1), and
        // no client can see another zone's spot — so the spot says so where the runner can read it.
        logger.LogInformation(
            "zone spot: announcement delivered. zone={ZoneId}, announcement={AnnouncementId}",
            ZoneId,
            announce.AnnouncementId);

        await PushToClientsAsync(
            ZoneTickUseCase.Humans(_state),
            new DeliverWorldAnnounceMsg(announce.AnnouncementId, announce.Text),
            cancellationToken);
    }

    /// <summary>
    /// Sends to each global ActorId. The Actor handler uses that Actor's current
    /// bound session. Bots never appear in <paramref name="playerIds"/> because they
    /// have no client session (ZW-F3).
    /// </summary>
    private async ValueTask PushToClientsAsync<TMessage>(
        IReadOnlyList<string> playerIds,
        TMessage message,
        CancellationToken cancellationToken)
    {
        foreach (var playerId in playerIds)
        {
            try
            {
                // PlayerId is the global ActorId. Resolve the current owner for every
                // delivery instead of retaining an Actor instance from a lifecycle callback.
                await actors
                    .SendToActor(playerId, message)
                    .Async(cancellationToken);
            }
            catch (Exception error)
            {
                // One unavailable Actor route must not prevent delivery to the other
                // players or terminate the periodic Spot timer.
                logger.LogWarning(
                    error,
                    "actor delivery skipped. zone={ZoneId}, player={PlayerId}",
                    ZoneId,
                    playerId);
            }
        }
    }

}
