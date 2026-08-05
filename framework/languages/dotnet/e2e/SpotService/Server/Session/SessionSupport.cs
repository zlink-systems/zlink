using System.Collections.Concurrent;
using Zlink.Framework.Contracts.Streams;

namespace SpotService.Server.Session;

using Zlink.Framework.E2E.Configuration;

internal sealed class EvidenceStore
{
    private readonly ConcurrentQueue<string> _entries = new();
    private readonly object _fileGate = new();
    private readonly string? _filePath;
    // A pulse completed on every Add and swapped for a fresh one, so EVERY
    // concurrent waiter wakes — a counted semaphore hands one release to one
    // waiter and silently starves the rest.
    private readonly object _pulseGate = new();
    private TaskCompletionSource<bool> _pulse =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public EvidenceStore(string rid, string? filePath)
    {
        Rid = rid;
        _filePath = filePath;
        if (!string.IsNullOrWhiteSpace(_filePath))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
            File.WriteAllText(_filePath, string.Empty);
        }
    }

    public string Rid { get; }

    public void Add(string entry)
    {
        _entries.Enqueue(entry);
        TaskCompletionSource<bool> pulse;
        lock (_pulseGate)
        {
            pulse = _pulse;
            _pulse = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
        }

        pulse.TrySetResult(true);
        if (string.IsNullOrWhiteSpace(_filePath)) return;

        lock (_fileGate)
        {
            File.AppendAllText(_filePath, entry + Environment.NewLine);
        }
    }

    public string[] Snapshot()
    {
        return _entries.ToArray();
    }

    public async Task<string[]> WaitUntilAsync(
        Func<string[], bool> condition,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (true)
        {
            Task pulseTask;
            lock (_pulseGate)
            {
                pulseTask = _pulse.Task;
            }

            var snapshot = Snapshot();
            if (condition(snapshot)) return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero) throw new TimeoutException("Timed out waiting for spot service evidence.");

            await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken));
        }
    }
}

internal sealed record NodeOptions(string Rid);

internal sealed class SessionBindingProbeStore
{
    private readonly ConcurrentDictionary<(string SessionId, string ActorId), IZLinkSessionActor>
        _bindings = new();

    public void Record(string sessionId, IZLinkSessionActor actor)
    {
        _bindings[(sessionId, actor.ActorId)] = actor;
    }

    public IZLinkSessionActor Require(string sessionId, string actorId)
    {
        return _bindings.TryGetValue((sessionId, actorId), out var actor)
            ? actor
            : throw new InvalidOperationException(
                $"Session '{sessionId}' did not preserve Actor '{actorId}' binding.");
    }
}

internal sealed record ServerOptions(
    string Role,
    string Rid,
    string HttpUrl,
    string LogDir,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null,
    string? ControlEndpoint = null,
    string? ControlPeerAEndpoint = null,
    string? ControlPeerBEndpoint = null,
    string? SpotRouterEndpoint = null,
    string? SpotRouterAdvertiseHost = null,
    string? SpotPeerAEndpoint = null,
    string? SpotPeerBEndpoint = null,
    string? SpotPubEndpoint = null,
    string? ExternalClientEndpoint = null,
    string? ExternalSpotEndpoint = null,
    string? ClientSpotPubEndpoint = null,
    string? StreamEndpoint = null,
    string? TlsStreamEndpoint = null,
    string? TlsCertPath = null,
    string? TlsKeyPath = null,
    string? MultiRouteAEndpoint = null,
    string? MultiRouteBEndpoint = null,
    string? MultiSpotRouterAEndpoint = null,
    string? MultiSpotRouterBEndpoint = null)
{
    public static ServerOptions Parse(string[] args, string defaultRole)
        => E2eConfiguration.Load<ServerOptions>(args) with { Role = defaultRole };
}
