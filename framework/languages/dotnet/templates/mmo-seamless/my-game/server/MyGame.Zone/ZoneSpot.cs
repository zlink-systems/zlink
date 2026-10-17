using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Zone;

public sealed class ZoneSpot(IZLinkSpotContext context, IZLinkActorClient actors)
    : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;

    // This projection supports visibility. The Actor remains the owner of player state.
    private readonly Dictionary<string, EnterZone> _visiblePlayers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, EnterZone> _arrivals = new(StringComparer.Ordinal);

    public void Configure()
    {
        foreach (var adjacent in GameNames.AdjacentZones(Context.SpotId))
            Context.Handlers.AddSubscribe<BorderHandler>(
                GameNames.BorderChannel,
                GameNames.BorderTopic(adjacent, Context.SpotId)
            );
    }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken) =>
        _ = await Context.AddTimer<BorderTick>(
            "border-tick",
            TimeSpan.FromMilliseconds(GameNames.TickPeriodMilliseconds),
            cancellationToken: cancellationToken
        );

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        Zlink.Framework.Contracts.Messaging.ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        var arrival = request.Decode<EnterZone>();
        if (GameNames.ZoneId(arrival.X) != Context.SpotId)
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());
        _arrivals.Add(actorId, arrival);
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        if (!_arrivals.Remove(actor.Context.ActorId, out var arrival))
            throw new InvalidDataException("Zone admission has no movement intent.");
        actor.Apply(arrival);
        Remember(actor);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        _visiblePlayers.Remove(actor.Context.ActorId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken
    ) => OnLeaveActorAsync(actor, cancellationToken);

    public void Remember(PlayerActor actor) =>
        _visiblePlayers[actor.Context.ActorId] = new EnterZone(actor.X, actor.Sequence);

    public async ValueTask PublishBorderAsync(CancellationToken cancellationToken)
    {
        foreach (var (actorId, position) in _visiblePlayers)
        {
            var offset = position.X % GameNames.ZoneWidth;
            if (
                offset >= GameNames.BorderRadius
                && offset < GameNames.ZoneWidth - GameNames.BorderRadius
            )
                continue;
            foreach (var adjacent in GameNames.AdjacentZones(Context.SpotId))
                await Context
                    .Outbound.Publish(
                        GameNames.BorderChannel,
                        GameNames.BorderTopic(Context.SpotId, adjacent),
                        new BorderPresence(Context.SpotId, actorId, position.X, position.Sequence)
                    )
                    .Async(cancellationToken);
        }
    }

    public async ValueTask ShowBorderAsync(BorderPresence presence)
    {
        foreach (var (observerId, observer) in _visiblePlayers)
        {
            if (Math.Abs(observer.X - presence.X) > GameNames.BorderRadius)
                continue;
            await actors
                .SendToActor(
                    observerId,
                    new BorderVisible(
                        Context.SpotId,
                        presence.SourceZoneId,
                        presence.ActorId,
                        presence.X,
                        presence.Sequence
                    )
                )
                .Async();
        }
    }

    public ValueTask DeliverAsync(PlayerActor actor, BorderVisible presence)
    {
        // Membership or visibility may change before a queued notification is handled.
        if (
            presence.ObserverZoneId != actor.Context.SpotId
            || !_visiblePlayers.ContainsKey(actor.Context.ActorId)
        )
            return ValueTask.CompletedTask;
        return actor.Context.BoundSession.Send(presence).Async();
    }

    public ZoneSnapshot Capture() => new(new(_visiblePlayers), new(_arrivals));

    public void Restore(ZoneSnapshot state)
    {
        _visiblePlayers.Clear();
        foreach (var (id, position) in state.VisiblePlayers)
            _visiblePlayers.Add(id, position);
        _arrivals.Clear();
        foreach (var (id, position) in state.Arrivals)
            _arrivals.Add(id, position);
    }
}

public sealed record ZoneSnapshot(
    Dictionary<string, EnterZone> VisiblePlayers,
    Dictionary<string, EnterZone> Arrivals
);

public sealed class ZoneRelocation : IZLinkSpotRelocationAdapter<ZoneSpot>
{
    public ValueTask<byte[]> CaptureAsync(ZoneSpot spot, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(spot.Capture()));
    }

    public ValueTask RestoreAsync(
        ZoneSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        spot.Restore(
            JsonSerializer.Deserialize<ZoneSnapshot>(payload.Span)
                ?? throw new InvalidDataException("Missing zone relocation state.")
        );
        return ValueTask.CompletedTask;
    }
}
