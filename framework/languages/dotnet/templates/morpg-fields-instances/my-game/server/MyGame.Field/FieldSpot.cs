using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Field;

public sealed class FieldSpot(
    IZLinkSpotContext context,
    IZLinkActorClient actors,
    IZLinkSpotManager spots
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;

    // This projection supports visibility. The Actor remains the owner of player state.
    private readonly Dictionary<string, EnterField> _visiblePlayers = new(StringComparer.Ordinal);
    private readonly FieldPositionDomain positions = new(context.SpotId);

    public void Configure()
    {
        Context.Handlers.AddActorPacket<ReconnectHandler, PlayerActor>(nameof(JoinWorldRequest));
        Context.Handlers.AddActorPacket<MoveHandler, PlayerActor>(nameof(MoveRequest));
        Context.Handlers.AddActorPacket<BorderDelivery, PlayerActor>(nameof(BorderVisible));
        Context.Handlers.AddActorPacket<CreateParty, PlayerActor>(nameof(CreatePartyRequest));
        Context.Handlers.AddActorPacket<JoinParty, PlayerActor>(nameof(JoinPartyRequest));
        Context.Handlers.AddActorPacket<EnterDungeon, PlayerActor>(nameof(EnterDungeonRequest));
        Context.Handlers.AddActorPacket<DungeonTravel, PlayerActor>(nameof(JoinDungeon));
        Context.Handlers.AddActorPacket<PartyDelivery, PlayerActor>(nameof(PartyView));
        Context.Handlers.AddActorPacket<FieldQuery, PlayerActor>(nameof(ViewRequest));
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
        var rejection = positions.QueueArrival(actorId, request.Decode<EnterField>());
        return ValueTask.FromResult(rejection is null
            ? ZLinkSpotActorJoinResult.Accept() : ZLinkSpotActorJoinResult.Reject());
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        if (!positions.TryTakeArrival(actor.Context.ActorId, out var arrival))
            throw new InvalidDataException("Field admission has no movement intent.");
        positions.AddPlayer(actor.Context.ActorId, arrival);
        actor.Apply(arrival);
        Remember(actor);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        _visiblePlayers.Remove(actor.Context.ActorId);
        positions.RemovePlayer(actor.Context.ActorId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        _visiblePlayers.Remove(actor.Context.ActorId);
        positions.SetConnected(actor.Context.ActorId, false);
        return ValueTask.CompletedTask;
    }

    public void Remember(PlayerActor actor)
    {
        positions.SetConnected(actor.Context.ActorId, true);
        _visiblePlayers[actor.Context.ActorId] = new EnterField(actor.X, actor.Sequence);
    }

    public async ValueTask MoveAsync(PlayerActor actor, MoveRequest request)
    {
        var result = positions.Move(actor.Context.ActorId, request);
        if (result.Rejection is { } rejection)
        {
            await actor.Context.BoundSession.Send(rejection).Async();
            return;
        }
        var arrival = new EnterField(result.Position.X, result.Sequence, result.RemainingMs);
        if (result.Relocates)
            actor.Move(arrival);
        else
        {
            actor.Apply(arrival);
            Remember(actor);
            await actor.SendStateAsync();
        }
    }

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

    public async ValueTask PartyAsync(PlayerActor actor, string? partyId, bool create, CancellationToken ct)
    {
        if (create)
        {
            if (actor.PartyId is not null)
            {
                await actor.Context.BoundSession.Send(new ActionRejected("invalid-party")).Async();
                return;
            }
            partyId = "party-" + Guid.NewGuid().ToString("N");
            var created = await spots.GetOrCreate(partyId, GameNames.PartyType)
                .InMesh(GameNames.WorldMesh).Async(ct);
            if (created.State == ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException("Party creation rejected.");
        }
        if (actor.PartyId is not null || !PartyRun.ValidId(partyId, "party-")
            || (!create && await spots.FindAsync(partyId!, ct) is null))
        {
            await actor.Context.BoundSession.Send(new ActionRejected("invalid-party")).Async();
            return;
        }
        var response = await Context.Outbound.RequestToSpot(partyId!,
            new PartyCommand(actor.Context.ActorId, Join: true)).Async<PartyCommandResult>(ct);
        if (response.Rejection is { } rejection)
            await actor.Context.BoundSession.Send(rejection).Async();
        else
            actor.PartyId = partyId;
    }

    public async ValueTask EnterAsync(PlayerActor actor, CancellationToken ct)
    {
        if (actor.PartyId is null)
        {
            await actor.Context.BoundSession.Send(new ActionRejected("not-leader")).Async();
            return;
        }
        var result = await Context.Outbound.RequestToSpot(actor.PartyId,
            new PartyCommand(actor.Context.ActorId, Enter: true)).Async<PartyCommandResult>(ct);
        if (result.Rejection is { } rejection)
            await actor.Context.BoundSession.Send(rejection).Async();
    }

    public async ValueTask ShowAsync(PlayerActor actor, CancellationToken ct)
    {
        await actor.SendStateAsync();
        if (actor.PartyId is null) return;
        var result = await Context.Outbound.RequestToSpot(actor.PartyId,
            new PartyCommand(actor.Context.ActorId)).Async<PartyCommandResult>(ct);
        if (result.View is { } view) await actor.Context.BoundSession.Send(view).Async();
    }

    public FieldSnapshot Capture() => new(new(_visiblePlayers), positions.Capture());
    public void Restore(FieldSnapshot state)
    {
        positions.Restore(state.Positions);
        _visiblePlayers.Clear();
        foreach (var (id, position) in state.VisiblePlayers) _visiblePlayers.Add(id, position);
    }

}

public sealed record FieldSnapshot(Dictionary<string, EnterField> VisiblePlayers,
    FieldPositionSnapshot Positions);

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
