using System.Text.Json;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Actors;

// --8<-- [start:doc-relocation-adapter]
internal sealed class PlayActorRelocationAdapter
    : IZLinkActorRelocationAdapter<PlayActor>
{
    public ValueTask<byte[]> CaptureAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(
            new PlayActorRelocationState(
            actor.RoomId,
            actor.Player,
            actor.DestroyAfterEntrySpotJoin,
            actor.Disconnected,
            actor.ProcessedJoinOperations)));
    }

    public ValueTask RestoreAsync(
        PlayActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var transferred = JsonSerializer.Deserialize<PlayActorRelocationState>(
            payload.Span) ?? throw new InvalidDataException("Actor relocation state is empty.");
        if (transferred.Player is not null) actor.ApplyPlayer(transferred.Player);
        if (!string.IsNullOrEmpty(transferred.RoomId)) actor.JoinRoom(transferred.RoomId);
        if (transferred.DestroyAfterEntrySpotJoin) actor.MarkForDestroyAfterRoomLeave();
        if (transferred.Disconnected) actor.MarkDisconnected();
        actor.RestoreProcessedJoinOperations(transferred.ProcessedJoinOperations);
        return ValueTask.CompletedTask;
    }

    private sealed record PlayActorRelocationState(
        string RoomId,
        PlayerInfo? Player,
        bool DestroyAfterEntrySpotJoin,
        bool Disconnected,
        IReadOnlyCollection<ZLinkActorJoinOperationId> ProcessedJoinOperations);
}
// --8<-- [end:doc-relocation-adapter]
