using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Zone;

public sealed class ZoneSpot(
    IZLinkSpotContext context,
    IZLinkActorClient actors,
    NodeSettings settings
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;

    private readonly ZonePositionDomain positions = new(context.SpotId);

    public void Configure()
    {
        foreach (var adjacent in GameNames.AdjacentZones(Context.SpotId))
            Context.Handlers.AddSubscribe<BorderHandler>(
                GameNames.BorderChannel,
                GameNames.BorderTopic(adjacent, Context.SpotId)
            );
    }

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        Zlink.Framework.Contracts.Messaging.ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var arrival = request.Decode<EnterZone>();
        var rejection = positions.QueueArrival(actorId, arrival);
        return ValueTask.FromResult(
            rejection is null
                ? ZLinkSpotActorJoinResult.Accept(
                    new WorldState(
                        Context.SpotId,
                        settings.Id,
                        arrival.Position.X,
                        arrival.Sequence
                    )
                )
                : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!positions.TryTakeArrival(actor.Context.ActorId, out var arrival))
            throw new InvalidDataException("Zone admission has no movement intent.");
        positions.AddPlayer(actor.Context.ActorId, arrival);
        await PublishLocalAsync(
            actor.Context.ActorId,
            new PlayerAppeared(actor.Context.ActorId, arrival.Position)
        );
        await PublishBorderAsync(
            actor.Context.ActorId,
            arrival.Position,
            arrival.Sequence,
            BorderPresenceKind.Appeared,
            null,
            cancellationToken
        );
    }

    public async ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var previous = positions.RemovePlayer(actor.Context.ActorId);
        if (previous is null)
            return;
        await PublishLeftAsync(
            actor.Context.ActorId,
            previous.Position,
            previous.Sequence,
            PlayerLeftReason.LeftZone,
            cancellationToken
        );
    }

    public async ValueTask OnDisconnectActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var state = positions.SetConnected(actor.Context.ActorId, false);
        if (state is null)
            return;
        await PublishLeftAsync(
            actor.Context.ActorId,
            state.Position,
            state.Sequence,
            PlayerLeftReason.Disconnected,
            cancellationToken
        );
    }

    public async ValueTask ReconnectAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        if (!positions.TryState(actor.Context.ActorId, out var previous))
        {
            await actor.Context.BoundSession.Send(new ActionRejected("outside-world", 0)).Async();
            return;
        }
        var state = positions.SetConnected(actor.Context.ActorId, true)!;
        if (!previous.Connected)
        {
            await PublishLocalAsync(
                actor.Context.ActorId,
                new PlayerAppeared(actor.Context.ActorId, state.Position)
            );
            await PublishBorderAsync(
                actor.Context.ActorId,
                state.Position,
                state.Sequence,
                BorderPresenceKind.Appeared,
                null,
                cancellationToken
            );
        }
        await SendWorldStateAsync(actor);
    }

    public async ValueTask MoveAsync(
        PlayerActor actor,
        MoveRequest request,
        CancellationToken cancellationToken
    )
    {
        var actorId = actor.Context.ActorId;
        var movement = positions.Move(actorId, request);
        if (!movement.Accepted)
        {
            await actor.Context.BoundSession.Send(movement.Rejection!).Async();
            return;
        }
        if (movement.Relocates)
        {
            actor.Move(
                movement.ZoneId,
                new EnterZone(movement.Position, movement.Sequence, movement.RemainingMs)
            );
            return;
        }
        await PublishLocalAsync(
            actorId,
            new PlayerMoved(actorId, movement.Position, movement.Sequence)
        );
        await PublishBorderAsync(
            actorId,
            movement.Position,
            movement.Sequence,
            BorderPresenceKind.Moved,
            null,
            cancellationToken
        );
        await SendWorldStateAsync(actor);
    }

    public async ValueTask ShowBorderAsync(BorderPresence presence)
    {
        foreach (var (observerId, observer) in positions.ConnectedPlayers())
        {
            if (
                observerId == presence.PlayerId
                || Math.Abs(observer.Position.X - presence.Position.X) > GameNames.BorderRadius
            )
                continue;
            if (presence.Kind == BorderPresenceKind.Appeared)
            {
                await actors.SendToActor(
                    observerId,
                    new PlayerAppeared(presence.PlayerId, presence.Position)
                ).Async();
            }
            else if (presence.Kind == BorderPresenceKind.Moved)
            {
                await actors.SendToActor(
                    observerId,
                    new PlayerMoved(presence.PlayerId, presence.Position, presence.Sequence)
                ).Async();
            }
            else if (presence.Kind == BorderPresenceKind.Left)
            {
                await actors.SendToActor(
                    observerId,
                    new PlayerLeft(
                        presence.PlayerId,
                        presence.Reason ?? PlayerLeftReason.LeftZone
                    )
                ).Async();
            }
            else
                throw new InvalidDataException("Unknown border update.");
        }
    }

    public ZonePositionSnapshot Capture() => positions.Capture();

    public void Restore(ZonePositionSnapshot state) => positions.Restore(state);

    private async ValueTask PublishLocalAsync<TMessage>(string playerId, TMessage message)
        where TMessage : notnull
    {
        foreach (var observerId in positions.ConnectedPlayers().Keys)
        {
            if (observerId == playerId)
                continue;
            await actors.SendToActor(observerId, message).Async();
        }
    }

    private async ValueTask PublishLeftAsync(
        string playerId,
        Position position,
        long sequence,
        PlayerLeftReason reason,
        CancellationToken cancellationToken
    )
    {
        await PublishLocalAsync(playerId, new PlayerLeft(playerId, reason));
        await PublishBorderAsync(
            playerId,
            position,
            sequence,
            BorderPresenceKind.Left,
            reason,
            cancellationToken
        );
    }

    private async ValueTask PublishBorderAsync(
        string playerId,
        Position position,
        long sequence,
        BorderPresenceKind kind,
        PlayerLeftReason? reason,
        CancellationToken cancellationToken
    )
    {
        var offset = position.X % GameNames.ZoneWidth;
        if (offset >= GameNames.BorderRadius && offset < GameNames.ZoneWidth - GameNames.BorderRadius)
            return;
        foreach (var adjacent in GameNames.AdjacentZones(Context.SpotId))
            await Context
                .Outbound.Publish(
                    GameNames.BorderChannel,
                    GameNames.BorderTopic(Context.SpotId, adjacent),
                    new BorderPresence(Context.SpotId, playerId, position, sequence, kind, reason)
                )
                .Async(cancellationToken);
    }

    private ValueTask SendWorldStateAsync(PlayerActor actor)
    {
        var state = positions.State(actor.Context.ActorId);
        return actor.Context.BoundSession
            .Send(new WorldState(Context.SpotId, settings.Id, state.Position.X, state.Sequence))
            .Async();
    }
}

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
            JsonSerializer.Deserialize<ZonePositionSnapshot>(payload.Span)
                ?? throw new InvalidDataException("Missing zone relocation state.")
        );
        return ValueTask.CompletedTask;
    }
}
