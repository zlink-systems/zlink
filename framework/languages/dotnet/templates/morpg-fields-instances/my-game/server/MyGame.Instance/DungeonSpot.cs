using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Instance;

public sealed class DungeonSpot(
    IZLinkSpotContext context,
    NodeSettings settings,
    IZLinkActorClient actors
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private DungeonState state = new();

    public void Configure()
    {
        Context.Handlers.AddActorPacket<DepartedFieldView, PlayerActor>(nameof(FieldView));
        Context.Handlers.AddActorPacket<DepartedBorderVisible, PlayerActor>(nameof(BorderVisible));
        Context.Handlers.AddActorPacket<DungeonReconnect, PlayerActor>(nameof(JoinWorldRequest));
        Context.Handlers.AddActorPacket<DungeonQuery, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<Advance, PlayerActor>(nameof(AdvanceRequest));
        Context.Handlers.AddActorPacket<DungeonDelivery, PlayerActor>(nameof(DungeonView));
    }

    public DungeonPrepared Prepare(PrepareDungeon request)
    {
        if (
            state.PartyId.Length != 0
            || string.IsNullOrWhiteSpace(request.PartyId)
            || request.Members.Length == 0
            || request.Members.Length > GameNames.MaximumPartyMembers
            || request.Members.Distinct().Count() != request.Members.Length
            || request.Members.Any(string.IsNullOrWhiteSpace)
            || GameNames.DungeonId(request.PartyId) != Context.SpotId
        )
            throw new InvalidDataException("Invalid dungeon party preparation.");
        state.PartyId = request.PartyId;
        state.Members = request.Members;
        return new DungeonPrepared(Context.SpotId);
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var join = request.Decode<JoinDungeon>();
        return ValueTask.FromResult(
            join.DungeonId == Context.SpotId
            && join.PartyId == state.PartyId
            && state.Members.Contains(actorId)
                ? ZLinkSpotActorJoinResult.Accept()
                : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        actors.SendToActor(actor.Context.ActorId, View()).Async();

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private DungeonView View() =>
        new(Context.SpotId, settings.Id, state.PartyId, state.Members, state.Stage);

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    public async ValueTask AdvanceAsync(PlayerActor requester, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        state.Stage = checked(state.Stage + 1);
        var view = View();
        await requester.Context.BoundSession.Send(view).Async();
        foreach (var id in state.Members)
            if (id != requester.Context.ActorId)
                await actors.SendToActor(id, view).Async();
    }

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);

    public void Restore(ReadOnlyMemory<byte> payload) =>
        state =
            JsonSerializer.Deserialize<DungeonState>(payload.Span)
            ?? throw new InvalidDataException("Missing dungeon relocation state.");
}

public sealed class DungeonState
{
    public string PartyId { get; set; } = "";
    public string[] Members { get; set; } = [];
    public int Stage { get; set; }
}

[ZLinkSpotRequestHandler(nameof(PrepareDungeon))]
public sealed class Prepare : IZLinkSpotRequestHandler<DungeonSpot, PrepareDungeon, DungeonPrepared>
{
    public ValueTask<DungeonPrepared> HandleAsync(
        DungeonSpot spot,
        PrepareDungeon request,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Prepare(request));
    }
}

public sealed class DungeonRelocation : IZLinkSpotRelocationAdapter<DungeonSpot>
{
    public ValueTask<byte[]> CaptureAsync(DungeonSpot spot, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Capture());
    }

    public ValueTask RestoreAsync(
        DungeonSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        spot.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
