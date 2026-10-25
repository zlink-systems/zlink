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
        if (join.PlayerId != actorId || ChannelRules.Validate(join.Channel) is not null || GameNames.TownId(join.Channel) != Context.SpotId)
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
            party.Value.Leave(id);
            if (party.Value.Members.Count == 0)
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
                .Select(pair => pair.Value.View())
                .ToArray()
        );

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    public ValueTask ShowPartyAsync(string playerId) =>
        actors.SendToActor(playerId, View()).Async();

    private async ValueTask BroadcastAsync(CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var view = View();
        foreach (var id in state.Players)
            await actors.SendToActor(id, view).Async();
    }

    public async ValueTask PartyAsync(
        PlayerActor actor,
        string? partyId,
        bool create,
        CancellationToken ct
    )
    {
        var id = actor.Context.ActorId;
        if (!state.Players.Contains(id)
            || state.Parties.Values.Any(party => party.Members.Contains(id))
            || (!create && (partyId is null || !state.Parties.TryGetValue(partyId, out var existing)
                || existing.Phase != RunPhase.Forming || existing.Members.Count >= GameNames.MaximumPartyMembers)))
        {
            await actor.Context.BoundSession.Send(new ActionRejected(Rejection.InvalidParty)).Async();
            return;
        }
        if (create)
        {
            var party = new PartyRun { Members = [id] };
            state.Parties.Add(party.PartyId, party);
        }
        else state.Parties[partyId!].Members.Add(id);
        await BroadcastAsync(ct);
    }

    public async ValueTask EnterAsync(PlayerActor actor, CancellationToken ct)
    {
        var party = state.Parties.Values.FirstOrDefault(item => item.Members.Contains(actor.Context.ActorId));
        var reason = party?.Enter(actor.Context.ActorId) ?? (party is null ? Rejection.NotLeader : null);
        if (reason is not null)
        {
            await actor.Context.BoundSession.Send(new ActionRejected(reason)).Async();
            return;
        }
        if (party!.Phase == RunPhase.Active) { await ShowAsync(actor); return; }
        var runId = party.RunId!;
        var dungeonId = GameNames.DungeonId(runId);
        var created = await spots.GetOrCreate(dungeonId, GameNames.DungeonType)
            .InMesh(GameNames.WorldMesh).Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Dungeon creation rejected.");
        var prepared = await Context.Outbound.RequestToSpot(dungeonId,
            new PrepareDungeon(runId, party.PartyId, Context.SpotId, party.Members.ToArray()))
            .Async<DungeonPrepared>(ct);
        if (prepared.Reason is not null || prepared.DungeonId != dungeonId || party.Prepared(prepared.RunId) is not null)
        {
            await actor.Context.BoundSession.Send(new ActionRejected(Rejection.RunMismatch)).Async();
            return;
        }
        foreach (var id in party.Members.Where(state.Players.Contains))
            await actors.SendToActor(id, new JoinDungeon(dungeonId, runId)).Async();
    }

    public ValueTask ChangeChannelAsync(PlayerActor actor, long channel)
    {
        var reason = ChannelRules.Validate(channel)
            ?? state.Parties.Values.FirstOrDefault(party => party.Members.Contains(actor.Context.ActorId))?.ChangeChannel();
        if (reason is not null) return actor.Context.BoundSession.Send(new ActionRejected(reason)).Async();
        actor.Context.JoinSpot(GameNames.TownId(channel), new JoinTownRequest(actor.Context.ActorId, channel)).Defer();
        return ValueTask.CompletedTask;
    }

    public async ValueTask RunStartedAsync(RunStarted message)
    {
        var party = state.Parties.Values.SingleOrDefault(item => item.RunId == message.RunId);
        if (party is null || party.Started(message.RunId) is not null)
            await RejectRunMismatchAsync(party);
        else
            await BroadcastAsync(CancellationToken.None);
    }

    public async ValueTask RunCompletedAsync(RunCompleted message)
    {
        var party = state.Parties.Values.SingleOrDefault(item => item.RunId == message.RunId);
        if (party is null || party.Completed(message.RunId) is not null)
            await RejectRunMismatchAsync(party);
        else
            await BroadcastAsync(CancellationToken.None);
    }

    private async ValueTask RejectRunMismatchAsync(PartyRun? party)
    {
        if (party?.Members.FirstOrDefault() is not { } leader) return;
        await actors.SendToActor(leader, new ActionRejected(Rejection.RunMismatch)).Async();
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
    public Dictionary<string, PartyRun> Parties { get; set; } = new(StringComparer.Ordinal);
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
