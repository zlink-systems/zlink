using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Field;

public sealed class FieldSpot(IZLinkSpotContext context, IZLinkActorClient actors, IZLinkSpotManager spots, NodeSettings settings)
    : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;

    // This projection supports visibility. The Actor remains the owner of player state.
    private readonly Dictionary<string, EnterField> _visiblePlayers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, EnterField> _arrivals = new(StringComparer.Ordinal);
    private Dictionary<string, List<string>> _parties = new(StringComparer.Ordinal);

    public void Configure()
    {
        Context.Handlers.AddActorPacket<ReconnectHandler, PlayerActor>(nameof(JoinWorldRequest));
        Context.Handlers.AddActorPacket<MoveHandler, PlayerActor>(nameof(MoveRequest));
        Context.Handlers.AddActorPacket<BorderDelivery, PlayerActor>(nameof(BorderVisible));
        Context.Handlers.AddActorPacket<CreateParty, PlayerActor>(nameof(CreatePartyRequest));
        Context.Handlers.AddActorPacket<JoinParty, PlayerActor>(nameof(JoinPartyRequest));
        Context.Handlers.AddActorPacket<EnterDungeon, PlayerActor>(nameof(EnterDungeonRequest));
        Context.Handlers.AddActorPacket<DungeonTravel, PlayerActor>(nameof(JoinDungeon));
        Context.Handlers.AddActorPacket<FieldDelivery, PlayerActor>(nameof(FieldView));
        foreach (var adjacent in GameNames.AdjacentFields(Context.SpotId))
            Context.Handlers.AddSubscribe<BorderHandler>(
                GameNames.BorderChannel,
                GameNames.BorderTopic(adjacent, Context.SpotId)
            );
    }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken) =>
        _ = await Context.AddTimer<BorderTick>(
            GameNames.BorderTimer,
            TimeSpan.FromMilliseconds(GameNames.TickPeriodMilliseconds),
            cancellationToken: cancellationToken
        );

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        Zlink.Framework.Contracts.Messaging.ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        var arrival = request.Decode<EnterField>();
        if (GameNames.FieldId(arrival.X) != Context.SpotId)
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());
        _arrivals.Add(actorId, arrival);
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        if (!_arrivals.Remove(actor.Context.ActorId, out var arrival))
            throw new InvalidDataException("Field admission has no movement intent.");
        actor.Apply(arrival);
        Remember(actor);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        _visiblePlayers.Remove(actor.Context.ActorId);
        foreach (var party in _parties)
        {
            if (!party.Value.Remove(actor.Context.ActorId)) continue;
            if (party.Value.Count == 0) _parties.Remove(party.Key);
            break;
        }
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken
    )
    {
        _visiblePlayers.Remove(actor.Context.ActorId);
        return ValueTask.CompletedTask;
    }

    public void Remember(PlayerActor actor) =>
        _visiblePlayers[actor.Context.ActorId] = new EnterField(actor.X, actor.Sequence);

    public async ValueTask PublishBorderAsync(CancellationToken cancellationToken)
    {
        foreach (var (actorId, position) in _visiblePlayers)
        {
            var offset = position.X % GameNames.FieldWidth;
            if (
                offset >= GameNames.BorderRadius
                && offset < GameNames.FieldWidth - GameNames.BorderRadius
            )
                continue;
            foreach (var adjacent in GameNames.AdjacentFields(Context.SpotId))
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
                        presence.SourceFieldId,
                        presence.PlayerId,
                        presence.X,
                        presence.Sequence
                    )
                )
                .Async();
        }
    }

    private FieldView View() => new(Context.SpotId, settings.Id, _visiblePlayers.Keys.Order().ToArray(),
        _parties.OrderBy(pair => pair.Key).Select(pair => new PartyView(pair.Key, pair.Value.ToArray())).ToArray());
    private async ValueTask BroadcastAsync(PlayerActor requester, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var view = View();
        await requester.Context.BoundSession.Send(view).Async();
        foreach (var id in _visiblePlayers.Keys)
            if (id != requester.Context.ActorId) await actors.SendToActor(id, view).Async();
    }
    public async ValueTask PartyAsync(PlayerActor actor, string partyId, bool create, CancellationToken ct)
    {
        var id = actor.Context.ActorId;
        if (string.IsNullOrWhiteSpace(partyId) || !_visiblePlayers.ContainsKey(id) ||
            _parties.Values.Any(members => members.Contains(id)) ||
            (create ? _parties.ContainsKey(partyId) :
                !_parties.TryGetValue(partyId, out var members) || members.Count >= GameNames.MaximumPartyMembers))
        {
            await actor.Context.BoundSession.Send(new ActionRejected(Rejection.InvalidParty)).Async();
            return;
        }
        if (create) _parties.Add(partyId, [id]);
        else _parties[partyId].Add(id);
        await BroadcastAsync(actor, ct);
    }
    public async ValueTask EnterAsync(PlayerActor actor, CancellationToken ct)
    {
        var party = _parties.FirstOrDefault(pair => pair.Value[0] == actor.Context.ActorId);
        if (party.Value is null)
        {
            await actor.Context.BoundSession.Send(new ActionRejected(Rejection.NotLeader)).Async();
            return;
        }
        var dungeonId = GameNames.DungeonId(party.Key);
        var created = await spots.GetOrCreate(dungeonId, GameNames.DungeonType).InMesh(GameNames.WorldMesh).Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Dungeon creation rejected.");
        var members = party.Value.ToArray();
        var prepared = await Context.Outbound.RequestToSpot(dungeonId, new PrepareDungeon(party.Key, members))
            .Async<DungeonPrepared>(ct);
        if (prepared.DungeonId != dungeonId) throw new InvalidDataException("Dungeon preparation identity mismatch.");
        foreach (var id in members) await actors.SendToActor(id, new JoinDungeon(dungeonId, party.Key)).Async();
    }

    public FieldSnapshot Capture() => new(new(_visiblePlayers), new(_arrivals), _parties);

    public void Restore(FieldSnapshot state)
    {
        _parties = state.Parties;
        _visiblePlayers.Clear();
        foreach (var (id, position) in state.VisiblePlayers)
            _visiblePlayers.Add(id, position);
        _arrivals.Clear();
        foreach (var (id, position) in state.Arrivals)
            _arrivals.Add(id, position);
    }
}

public sealed record FieldSnapshot(
    Dictionary<string, EnterField> VisiblePlayers,
    Dictionary<string, EnterField> Arrivals,
    Dictionary<string, List<string>> Parties
);

public sealed class FieldRelocation : IZLinkSpotRelocationAdapter<FieldSpot>
{
    public ValueTask<byte[]> CaptureAsync(FieldSpot spot, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(spot.Capture()));
    }

    public ValueTask RestoreAsync(
        FieldSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        spot.Restore(
            JsonSerializer.Deserialize<FieldSnapshot>(payload.Span)
                ?? throw new InvalidDataException("Missing field relocation state.")
        );
        return ValueTask.CompletedTask;
    }
}
