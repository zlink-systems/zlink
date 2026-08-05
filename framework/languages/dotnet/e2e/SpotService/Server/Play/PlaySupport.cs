using System.Collections.Concurrent;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.E2E.Configuration;

namespace SpotService.Server.Play;

internal sealed class ApplicationJoinCoordinator(
    IZLinkActorManager actors,
    EvidenceStore evidence)
{
    private readonly ConcurrentDictionary<string, Task> _operations = new(StringComparer.Ordinal);

    public void Start(JoinReq request)
    {
        _operations.GetOrAdd(request.ActorId, _ => RunAsync(request));
    }

    private async Task RunAsync(JoinReq request)
    {
        try
        {
            Task<ZLinkActorCreateResult> operation;
            using (ExecutionContext.SuppressFlow())
                operation = Task.Run(async () => await actors
                    .GetOrCreate(request.ActorId, SpotServiceNames.ActorType)
                    .Request(new ScenarioActorCreateReq(request.DisplayName))
                    .Async());
            var actor = await operation switch
            {
                ZLinkActorCreateResult.Existing value => value.Actor,
                ZLinkActorCreateResult.Created value => value.Actor,
                _ => throw new InvalidOperationException("Actor creation was rejected.")
            };
            evidence.Add(
                $"application-join-completed|actor={actor.ActorId}|node={actor.NodeRid}"
                + $"|generation={actor.ObjectGeneration}");
        }
        catch (Exception error)
        {
            evidence.Add(
                $"application-join-failed|actor={request.ActorId}|error={error.GetType().Name}"
                + $"|kind={(error as ZLinkFrameworkException)?.Kind.ToString() ?? "<none>"}"
                + $"|message={error.Message.ReplaceLineEndings(" ")}");
        }
        finally
        {
            _operations.TryRemove(request.ActorId, out _);
        }
    }
}

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

internal sealed record ServerOptions(
    string Role,
    string Rid,
    string HttpUrl,
    string LogDir,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null,
    string? ControlEndpoint = null,
    string? SpotRouterEndpoint = null,
    string? SpotRouterAdvertiseHost = null,
    string? SpotPubEndpoint = null,
    string? ExternalSpotEndpoint = null,
    string? ClientSpotPubEndpoint = null,
    string? StreamEndpoint = null,
    string? TlsStreamEndpoint = null,
    string? TlsCertPath = null,
    string? TlsKeyPath = null,
    string? MultiRouteAEndpoint = null,
    string? MultiRouteBEndpoint = null,
    string? MultiSpotRouterAEndpoint = null,
    string? MultiSpotRouterBEndpoint = null,
    int? MessageFollowDurationMilliseconds = null,
    int? OwnerLeaseTtlMilliseconds = null,
    int? PopulationLimit = null,
    int? InstanceSpotIdleTimeoutMilliseconds = null)
{
    public static ServerOptions Parse(string[] args, string defaultRole)
        => E2eConfiguration.Load<ServerOptions>(args) with { Role = defaultRole };
}
