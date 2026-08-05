using System.Text.Json;
using Zlink.Framework.Contracts.Actors;

namespace Bingo.Server.Play.Infrastructure.ZLink.Actors;

internal sealed class PlayerActorRelocationAdapter
    : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(
            new PlayerActorRelocationState(
            actor.DisplayName,
            actor.RoomId,
            actor.DestroyAfterEntrySpotJoin,
            actor.Disconnected,
            actor.LastCompletedJoinOperationId?.High,
            actor.LastCompletedJoinOperationId?.Low,
            actor.LastCompletedJoinOutcome,
            actor.PendingJoinsSnapshot()
                .Select(static pending => new PendingJoinState(
                    pending.RoomId,
                    pending.ObserveOnly))
                .ToArray())));
    }

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var transferred = JsonSerializer.Deserialize<PlayerActorRelocationState>(
            payload.Span) ?? throw new InvalidDataException("Actor relocation state is empty.");
        actor.SetDisplayName(transferred.DisplayName);
        if (!string.IsNullOrEmpty(transferred.RoomId)) actor.JoinRoom(transferred.RoomId);
        if (transferred.DestroyAfterEntrySpotJoin) actor.MarkForDestroyAfterRoomLeave();
        if (transferred.Disconnected) actor.MarkDisconnected();
        actor.RestoreJoinCompletion(
            transferred.JoinOperationHigh is { } high
            && transferred.JoinOperationLow is { } low
                ? new ZLinkActorJoinOperationId(high, low)
                : null,
            transferred.JoinOutcome);
        actor.RestorePendingJoins(
            transferred.PendingJoins.Select(static pending =>
                (pending.RoomId, pending.ObserveOnly)));
        return ValueTask.CompletedTask;
    }

    private sealed record PlayerActorRelocationState(
        string DisplayName,
        string RoomId,
        bool DestroyAfterEntrySpotJoin,
        bool Disconnected,
        ulong? JoinOperationHigh,
        ulong? JoinOperationLow,
        string JoinOutcome,
        PendingJoinState[] PendingJoins);

    private sealed record PendingJoinState(
        string RoomId,
        bool ObserveOnly);
}
