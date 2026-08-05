using System.Text.Json;
using Zlink.Framework.Contracts.Actors;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;

internal sealed class PlayerActorFactory : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new PlayerActor(context.ActorId, context));
    }
}

/// <summary>
/// Captures the player-owned coordinate and movement state for relocation.
/// </summary>
internal sealed class PlayerActorRelocationAdapter
    : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(
            new PlayerRelocationState(
                actor.Position.X,
                actor.Position.Y,
                actor.ZoneId,
                actor.IsBot,
                actor.DirX,
                actor.DirY,
                actor.ProcessedJoinOperations)));
    }

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (payload.IsEmpty)
            throw new InvalidOperationException(
                $"Player '{actor.Context.ActorId}' has no relocation state.");

        var restored = JsonSerializer.Deserialize<PlayerRelocationState>(
            payload.Span) ?? throw new InvalidDataException(
            "Player relocation state is empty.");
        actor.Restore(
            restored.X,
            restored.Y,
            restored.ZoneId,
            restored.IsBot,
            restored.DirX,
            restored.DirY);
        actor.RestoreProcessedJoinOperations(restored.ProcessedJoinOperations);
        return ValueTask.CompletedTask;
    }

    private sealed record PlayerRelocationState(
        int X,
        int Y,
        string ZoneId,
        bool IsBot,
        int DirX,
        int DirY,
        IReadOnlyCollection<ZLinkActorJoinOperationId> ProcessedJoinOperations);
}
