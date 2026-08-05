using RuntimeMonitoring.Shared;
using Zlink.Framework.Contracts.Configuration;

namespace RuntimeMonitoring.Server.Service.Support;

internal sealed class ObserverIsolationProbe(
    IZLinkRouteMeshRuntime runtime) : IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly TaskCompletionSource _releaseSlow =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly List<ulong> _normalSequences = [];
    private Task? _normalTask;
    private Task? _slowTask;
    private ulong _slowLatestSequence;
    private bool _slowReleased;
    private bool _slowFailed;

    public void Start(string meshName)
    {
        lock (_gate)
        {
            if (_normalTask is not null)
                return;
            _normalTask = Task.Run(
                () => ConsumeNormalAsync(meshName, _stop.Token));
            _slowTask = Task.Run(
                () => ConsumeSlowAsync(meshName, _stop.Token));
        }
    }

    public void ReleaseSlowConsumer()
    {
        lock (_gate)
            _slowReleased = true;
        _releaseSlow.TrySetResult();
    }

    public ObserverIsolationStatusRes Status()
    {
        lock (_gate)
        {
            var latestNormal = _normalSequences.Count == 0
                ? 0
                : _normalSequences[^1];
            var gap = _normalSequences
                .Zip(_normalSequences.Skip(1), static (left, right) => right > left + 1)
                .Any(static value => value);
            return new ObserverIsolationStatusRes(
                Running: _normalTask is not null,
                _slowReleased,
                _slowFailed,
                _normalSequences.Count,
                latestNormal,
                _slowLatestSequence,
                gap);
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _releaseSlow.TrySetResult();
        var tasks = new[] { _normalTask, _slowTask }
            .Where(static task => task is not null)
            .Cast<Task>()
            .ToArray();
        try
        {
            await Task.WhenAll(tasks).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
        _stop.Dispose();
    }

    private async Task ConsumeNormalAsync(
        string meshName,
        CancellationToken cancellationToken)
    {
        await foreach (var runtimeEvent in runtime.ObserveAsync(
                           meshName,
                           cancellationToken))
        {
            lock (_gate)
                _normalSequences.Add(runtimeEvent.Status.Sequence);
        }
    }

    private async Task ConsumeSlowAsync(
        string meshName,
        CancellationToken cancellationToken)
    {
        await using var events = runtime.ObserveAsync(
                meshName,
                cancellationToken)
            .GetAsyncEnumerator(cancellationToken);
        if (!await events.MoveNextAsync().ConfigureAwait(false))
            return;

        await _releaseSlow.Task.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        lock (_gate)
            _slowReleased = true;

        if (await events.MoveNextAsync().ConfigureAwait(false))
        {
            lock (_gate)
                _slowLatestSequence = events.Current.Status.Sequence;
        }

        try
        {
            throw new InvalidOperationException(
                "MON-C1 intentional slow consumer failure.");
        }
        catch (InvalidOperationException)
        {
            lock (_gate)
                _slowFailed = true;
        }
    }
}
