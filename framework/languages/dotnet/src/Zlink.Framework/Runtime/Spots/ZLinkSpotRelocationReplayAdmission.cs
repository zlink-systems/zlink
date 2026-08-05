namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkSpotRelocationReplayAdmission(
    ZLinkSpotActivation Activation,
    ZLinkSpotRelocationSeal Seal,
    string HandoffId,
    ZLinkSpotRelocationActorQueueReservation QueueReservation);

internal sealed class ZLinkSpotRelocationActorQueueReservation(
    string actorId)
{
    private readonly TaskCompletionSource<Func<CancellationToken, ValueTask>>
        _work = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private Task? _execution;
    private int _claimed;

    internal string ActorId { get; } = actorId;

    internal ValueTask RunAsync(CancellationToken cancellationToken) =>
        RunReservedAsync(cancellationToken);

    internal void BindExecution(Task execution)
    {
        ArgumentNullException.ThrowIfNull(execution);
        if (Interlocked.CompareExchange(ref _execution, execution, null)
            is not null)
            throw new InvalidOperationException(
                "SPOT relocation Actor queue reservation was already bound.");
    }

    internal async ValueTask ExecuteAsync(
        string candidateActorId,
        Func<CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(candidateActorId);
        ArgumentNullException.ThrowIfNull(operation);
        if (!string.Equals(ActorId, candidateActorId,
                StringComparison.Ordinal))
            throw new InvalidOperationException(
                "SPOT relocation Actor queue reservation changed Actor.");
        if (Interlocked.Exchange(ref _claimed, 1) != 0
            || !_work.TrySetResult(operation))
            throw new InvalidOperationException(
                "SPOT relocation Actor queue reservation was already consumed.");
        var execution = Volatile.Read(ref _execution)
                        ?? throw new InvalidOperationException(
                            "SPOT relocation Actor queue reservation is not bound.");
        await execution.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    internal void Discard()
    {
        if (Interlocked.Exchange(ref _claimed, 1) == 0)
            _work.TrySetResult(static _ => ValueTask.CompletedTask);
    }

    private async ValueTask RunReservedAsync(
        CancellationToken cancellationToken)
    {
        var operation = await _work.Task.ConfigureAwait(false);
        await operation(cancellationToken).ConfigureAwait(false);
    }
}

internal static class ZLinkSpotRelocationReplayScope
{
    private static readonly AsyncLocal<ZLinkSpotRelocationReplayAdmission?>
        CurrentAdmission = new();

    internal static ZLinkSpotRelocationReplayAdmission? Current =>
        CurrentAdmission.Value;

    internal static IDisposable Enter(
        ZLinkSpotRelocationReplayAdmission admission)
    {
        ArgumentNullException.ThrowIfNull(admission);
        var previous = CurrentAdmission.Value;
        CurrentAdmission.Value = admission;
        return new Scope(previous);
    }

    private sealed class Scope(
        ZLinkSpotRelocationReplayAdmission? previous) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                CurrentAdmission.Value = previous;
        }
    }
}
