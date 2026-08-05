using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Channels;

namespace AutomaticTurnDispatch.Server.Play;

using Zlink.Framework.E2E.Configuration;

internal sealed record NodeOptions(string Rid);

internal sealed class EvidenceStore
{
    private readonly List<string> _entries = [];
    private readonly string? _filePath;
    private readonly object _gate = new();
    private readonly List<TaskCompletionSource> _waiters = [];

    public EvidenceStore(string rid, string? filePath)
    {
        Rid = rid;
        _filePath = filePath;
    }

    public string Rid { get; }

    public void Add(string entry)
    {
        lock (_gate)
        {
            _entries.Add(entry);
            if (!string.IsNullOrWhiteSpace(_filePath)) File.AppendAllText(_filePath, entry + Environment.NewLine);

            foreach (var waiter in _waiters.ToArray()) waiter.TrySetResult();

            _waiters.Clear();
        }
    }

    public string[] Snapshot()
    {
        lock (_gate)
        {
            return _entries.ToArray();
        }
    }

    public string[] Snapshot(string requestId)
    {
        ArgumentException.ThrowIfNullOrEmpty(requestId);
        lock (_gate)
        {
            return _entries
                .Where(entry => entry.Contains(
                    $"request={requestId}",
                    StringComparison.Ordinal))
                .ToArray();
        }
    }

    public async Task<string[]> WaitUntilAsync(
        Func<string[], bool> predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (true)
        {
            TaskCompletionSource waiter;
            string[] snapshot;
            lock (_gate)
            {
                snapshot = _entries.ToArray();
                if (predicate(snapshot)) return snapshot;

                waiter = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                _waiters.Add(waiter);
            }

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero)
            {
                RemoveWaiter(waiter);
                throw new TimeoutException("Timed out waiting for await dispatch evidence.");
            }

            try
            {
                await waiter.Task.WaitAsync(remaining, cancellationToken);
            }
            catch
            {
                RemoveWaiter(waiter);
                throw;
            }
        }
    }

    private void RemoveWaiter(TaskCompletionSource waiter)
    {
        lock (_gate)
        {
            _waiters.Remove(waiter);
        }
    }
}

internal sealed record PlayOptions(
    string Rid,
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ControlEndpoint,
    string DelayEndpoint,
    string ExternalApiBaseUrl,
    string SpotRouterEndpoint,
    string SpotPubEndpoint,
    string SpotRouteEndpoint,
    string LogDir,
    int PlacementWeight = 100)
{
    public string EvidenceFile => Path.Combine(LogDir, $"{Rid}.evidence.log");

    public static PlayOptions Parse(string[] args)
        => E2eConfiguration.Load<PlayOptions>(args);
}

internal static class AwaitReplies
{
    public static AutomaticTurnDispatchRes Reply(
        string scenarioId,
        string requestId,
        AwaitProbeSpot spot,
        string marker)
    {
        return new AutomaticTurnDispatchRes(
            scenarioId,
            requestId,
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            marker);
    }
}

internal static class ActorReplies
{
    public static ActorAwaitRes Reply(
        string scenarioId,
        string requestId,
        AwaitActor actor,
        AwaitEntrySpot entrySpot,
        string marker)
    {
        return new ActorAwaitRes(
            scenarioId,
            requestId,
            actor.ActorId,
            entrySpot.Context.SpotId.ToString(),
            entrySpot.Context.NodeRid.ToString(),
            marker);
    }

    public static ActorAwaitRes Reply(
        string scenarioId,
        string requestId,
        AwaitActor actor,
        AwaitProbeSpot spot,
        string marker)
    {
        return new ActorAwaitRes(
            scenarioId,
            requestId,
            actor.ActorId,
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            marker);
    }
}

internal static class TurnTerminator
{
    public static ValueTask<TReply> Complete<TReply>(
        IZLinkRequestCall call,
        string terminator,
        CancellationToken cancellationToken)
    {
        return terminator switch
        {
            "async" => call.Async<TReply>(cancellationToken),
            "yield" => call.Yield<TReply>(cancellationToken),
            _ => throw new InvalidOperationException($"Unknown execution terminator '{terminator}'.")
        };
    }
}
