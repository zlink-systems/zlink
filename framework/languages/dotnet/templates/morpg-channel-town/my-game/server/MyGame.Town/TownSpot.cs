using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Town;

public sealed class TownSpot(
    IZLinkSpotContext context,
    NodeSettings settings,
    IZLinkActorClient actors,
    IZLinkSpotManager spots
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private TownState state = new();

    public void Configure()
    {
        Context.Handlers.AddActorPacket<TownReconnect, PlayerActor>(nameof(JoinTownRequest));
        Context.Handlers.AddActorPacket<ChangeChannel, PlayerActor>(nameof(ChangeChannelRequest));
        Context.Handlers.AddActorPacket<CreateParty, PlayerActor>(nameof(CreatePartyRequest));
        Context.Handlers.AddActorPacket<JoinParty, PlayerActor>(nameof(JoinPartyRequest));
        Context.Handlers.AddActorPacket<EnterDungeon, PlayerActor>(nameof(EnterDungeonRequest));
        Context.Handlers.AddActorPacket<DungeonTravel, PlayerActor>(nameof(JoinDungeon));
        Context.Handlers.AddActorPacket<TownQuery, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<TownDelivery, PlayerActor>(nameof(TownView));
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var join = request.Decode<JoinTownRequest>();
        if (join.PlayerId != actorId || GameNames.TownId(join.Channel) != Context.SpotId)
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());
        state.Players.Add(actorId);
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        BroadcastAsync(ct);

    public async ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct)
    {
        var id = actor.Context.ActorId;
        state.Players.Remove(id);
        foreach (var party in state.Parties.ToArray())
        {
            party.Value.Remove(id);
            if (party.Value.Count == 0)
                state.Parties.Remove(party.Key);
        }
        await BroadcastAsync(ct);
    }

    // A disconnected player retains the channel and party until an explicit move.
    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private TownView View() =>
        new(
            Context.SpotId,
            settings.Id,
            state.Players.Order().ToArray(),
            state
                .Parties.OrderBy(pair => pair.Key)
                .Select(pair => new PartyView(pair.Key, pair.Value.ToArray()))
                .ToArray()
        );

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    private async ValueTask BroadcastAsync(CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var view = View();
        foreach (var id in state.Players)
            await actors.SendToActor(id, view).Async();
    }

    public async ValueTask PartyAsync(
        PlayerActor actor,
        string partyId,
        bool create,
        CancellationToken ct
    )
    {
        var id = actor.Context.ActorId;
        if (
            string.IsNullOrWhiteSpace(partyId)
            || !state.Players.Contains(id)
            || state.Parties.Values.Any(members => members.Contains(id))
            || (
                create
                    ? state.Parties.ContainsKey(partyId)
                    : !state.Parties.TryGetValue(partyId, out var members)
                        || members.Count >= GameNames.MaximumPartyMembers
            )
        )
        {
            await actor
                .Context.BoundSession.Send(new ActionRejected(Rejection.InvalidParty))
                .Async();
            return;
        }
        if (create)
            state.Parties.Add(partyId, [id]);
        else
            state.Parties[partyId].Add(id);
        await BroadcastAsync(ct);
    }

    public async ValueTask EnterAsync(PlayerActor actor, CancellationToken ct)
    {
        var party = state.Parties.FirstOrDefault(pair => pair.Value[0] == actor.Context.ActorId);
        if (party.Value is null)
        {
            await actor.Context.BoundSession.Send(new ActionRejected(Rejection.NotLeader)).Async();
            return;
        }
        var dungeonId = GameNames.DungeonId(party.Key);
        var created = await spots
            .GetOrCreate(dungeonId, GameNames.DungeonType)
            .InMesh(GameNames.WorldMesh)
            .Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Dungeon creation rejected.");
        var members = party.Value.ToArray();
        var prepared = await Context
            .Outbound.RequestToSpot(dungeonId, new PrepareDungeon(party.Key, members))
            .Async<DungeonPrepared>(ct);
        if (prepared.DungeonId != dungeonId)
            throw new InvalidDataException("Dungeon preparation identity mismatch.");
        foreach (var id in members)
            await actors.SendToActor(id, new JoinDungeon(dungeonId, party.Key)).Async();
    }

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);

    public void Restore(ReadOnlyMemory<byte> payload) =>
        state =
            JsonSerializer.Deserialize<TownState>(payload.Span)
            ?? throw new InvalidDataException("Missing town relocation state.");
}

public sealed class TownState
{
    public HashSet<string> Players { get; set; } = new(StringComparer.Ordinal);
    public Dictionary<string, List<string>> Parties { get; set; } = new(StringComparer.Ordinal);
}

public sealed class TownRelocation : IZLinkSpotRelocationAdapter<TownSpot>
{
    public ValueTask<byte[]> CaptureAsync(TownSpot spot, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Capture());
    }

    public ValueTask RestoreAsync(TownSpot spot, ReadOnlyMemory<byte> payload, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        spot.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
