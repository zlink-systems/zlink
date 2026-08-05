using System.Collections.Concurrent;

namespace SubmitAdmission.Server.Infrastructure;

internal sealed class PendingCancellationRegistry : IDisposable
{
    private readonly ConcurrentDictionary<string, CancellationTokenSource> _sources =
        new(StringComparer.Ordinal);

    public void Register(string operationId, CancellationTokenSource source)
    {
        if (!_sources.TryAdd(operationId, source))
        {
            source.Dispose();
            throw new InvalidOperationException(
                $"Cancellation is already registered for operation '{operationId}'.");
        }
    }

    public bool Cancel(string operationId)
    {
        if (!_sources.TryRemove(operationId, out var source)) return false;
        try
        {
            source.Cancel();
            return true;
        }
        finally
        {
            source.Dispose();
        }
    }

    public void Complete(string operationId)
    {
        if (_sources.TryRemove(operationId, out var source)) source.Dispose();
    }

    public void Dispose()
    {
        foreach (var operationId in _sources.Keys)
            if (_sources.TryRemove(operationId, out var source)) source.Dispose();
    }
}
