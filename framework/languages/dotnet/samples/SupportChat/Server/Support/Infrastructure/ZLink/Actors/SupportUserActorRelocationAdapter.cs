using System.Text.Json;
using Zlink.Framework.Contracts.Actors;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Actors;

internal sealed class SupportUserActorRelocationAdapter
    : IZLinkActorRelocationAdapter<SupportUserActor>
{
    public ValueTask<byte[]> CaptureAsync(
        SupportUserActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(
            new SupportUserActorRelocationState(
            actor.DisplayName,
            actor.Role,
            actor.ParticipantId,
            actor.ConversationId,
            actor.CaptureCompletedJoinOperations())));
    }

    public ValueTask RestoreAsync(
        SupportUserActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var transferred = JsonSerializer.Deserialize<SupportUserActorRelocationState>(
            payload.Span) ?? throw new InvalidDataException("Actor relocation state is empty.");
        actor.SetIdentity(transferred.DisplayName, transferred.Role, transferred.ParticipantId);
        if (!string.IsNullOrEmpty(transferred.ConversationId))
            actor.JoinConversation(transferred.ConversationId);
        actor.RestoreCompletedJoinOperations(transferred.CompletedJoinOperations);
        return ValueTask.CompletedTask;
    }

    private sealed record SupportUserActorRelocationState(
        string DisplayName,
        string Role,
        string ParticipantId,
        string ConversationId,
        string[] CompletedJoinOperations);
}
