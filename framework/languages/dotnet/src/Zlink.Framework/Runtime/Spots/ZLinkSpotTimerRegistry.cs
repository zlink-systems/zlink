using Zlink.Framework.Runtime.Timers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkSpotLogicalTimerSnapshot(
    Type HandlerType,
    Type SpotType,
    ZLinkTimerLogicalSnapshot Timer);

internal sealed class ZLinkSpotTimerRegistry(
    Func<bool> flowCaptureEnabled,
    bool restorePending = false,
    ZLinkTimerScheduler? scheduler = null) : IAsyncDisposable
{
    private readonly ZLinkTimerScheduler _scheduler = scheduler ?? new();
    private readonly bool _ownsScheduler = scheduler is null;
    private readonly ZLinkStateLane _lane = new();
    private readonly List<ZLinkSpotTimerRegistration> _timers = [];
    private Task? _finalization;
    private bool _closed;
    private bool _frozen = restorePending;
    private bool _restorePending = restorePending;

    internal bool IsFrozen
        => AwaitStateLane(_lane.RunAsync(() => _frozen));

    public ValueTask DisposeAsync()
    {
        return new ValueTask(AwaitStateLane(_lane.RunAsync(GetOrStartFinalization)));
    }

    private Task GetOrStartFinalization()
    {
        if (_finalization is not null) return _finalization;

        _closed = true;
        var timers = _timers.ToArray();
        _timers.Clear();
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _finalization = completion.Task;
        using (ExecutionContext.SuppressFlow())
            _ = Task.Run(() => CompleteFinalizationAsync(timers, completion));
        return _finalization;
    }

    public ValueTask<IZLinkTimer> AddAsync(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options,
        Type handlerType,
        Type spotType,
        CancellationToken stopToken,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, CancellationToken, ValueTask<bool>> dispatchAsync,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, Exception, bool, CancellationToken, ValueTask>
            reportFailureAsync,
        CancellationToken cancellationToken)
    {
        return _lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            cancellationToken.ThrowIfCancellationRequested();
            if (_frozen && !_restorePending)
                throw new ZLinkConfigurationException(
                    "SPOT timer registration is sealed for relocation.");

            var timerOptions = ValidateRegistration(name, period, options);

            var descriptor = ZLinkSpotDescriptorFactory.CreateTimerDescriptor(name, period, handlerType, spotType);
            var timer = new ZLinkTimer(
                new ZLinkTimerLogicalSnapshot(
                    name,
                    period,
                    timerOptions,
                    DateTimeOffset.UtcNow,
                    0,
                    0,
                    null,
                    null),
                stopToken,
                (tick, ct) => dispatchAsync(descriptor, tick, ct),
                (tick, error, stopped, ct) =>
                    reportFailureAsync(descriptor, tick, error, stopped, ct),
                () => ZLinkFlowContext.Enter(
                    null,
                    null,
                    flowCaptureEnabled(),
                    ZLinkFlowOrigin.Timer),
                startFrozen: _restorePending,
                scheduler: _scheduler);
            _timers.Add(new ZLinkSpotTimerRegistration(
                timer,
                handlerType,
                spotType));
            return (IZLinkTimer)timer;
        });
    }

    internal IReadOnlyList<ZLinkSpotLogicalTimerSnapshot> Freeze()
        => AwaitStateLane(_lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            _frozen = true;
            return _timers
                .Where(static registration => !registration.Timer.IsDisposed)
                .Select(static registration => new ZLinkSpotLogicalTimerSnapshot(
                    registration.HandlerType,
                    registration.SpotType,
                    registration.Timer.Freeze()))
                .OrderBy(static snapshot => snapshot.Timer.Name, StringComparer.Ordinal)
                .ToArray();
        }));

    internal IReadOnlyList<ZLinkRelocationLogicalTimer> FreezeRelocation()
    {
        return Freeze()
            .Select(ZLinkSpotTimerRelocationCodec.Encode)
            .ToArray();
    }

    internal async ValueTask<IReadOnlyList<ZLinkRelocationLogicalTimer>>
        SnapshotFrozenRelocationAfterDispatchesAsync(
            CancellationToken cancellationToken)
    {
        var registrations = await _lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            if (!_frozen)
                throw new InvalidOperationException(
                    "SPOT logical timers must be frozen before relocation snapshot.");
            return _timers
                .Where(static registration => !registration.Timer.IsDisposed)
                .ToArray();
        }).ConfigureAwait(false);

        await Task.WhenAll(registrations.Select(
                static registration =>
                    registration.Timer.WaitForFrozenDispatchAsync()))
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);

        return await _lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            if (!_frozen)
                throw new InvalidOperationException(
                    "SPOT logical timers resumed before relocation snapshot.");
            return registrations
                .Where(static registration => !registration.Timer.IsDisposed)
                .Select(static registration =>
                    new ZLinkSpotLogicalTimerSnapshot(
                        registration.HandlerType,
                        registration.SpotType,
                        registration.Timer.Snapshot()))
                .OrderBy(
                    static snapshot => snapshot.Timer.Name,
                    StringComparer.Ordinal)
                .Select(ZLinkSpotTimerRelocationCodec.Encode)
                .ToArray();
        }).ConfigureAwait(false);
    }

    internal void FreezeForApplicationAdmissionSeal()
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_closed || _frozen) return;
            _frozen = true;
            foreach (var registration in _timers)
            {
                if (!registration.Timer.IsDisposed)
                    _ = registration.Timer.Freeze();
            }
        }));
    }

    internal void RestoreRelocation(
        IReadOnlyList<ZLinkRelocationLogicalTimer> logicalTimers,
        CancellationToken stopToken,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, CancellationToken, ValueTask<bool>> dispatchAsync,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, Exception, bool, CancellationToken, ValueTask>
            reportFailureAsync) =>
        RestoreRelocation(
            logicalTimers,
            typeof(object),
            stopToken,
            dispatchAsync,
            reportFailureAsync);

    internal void RestoreRelocation(
        IReadOnlyList<ZLinkRelocationLogicalTimer> logicalTimers,
        Type spotType,
        CancellationToken stopToken,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, CancellationToken, ValueTask<bool>> dispatchAsync,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, Exception, bool, CancellationToken, ValueTask>
            reportFailureAsync)
    {
        ArgumentNullException.ThrowIfNull(logicalTimers);
        AwaitStateLane(_lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            var names = new HashSet<string>(StringComparer.Ordinal);
            var snapshots = logicalTimers
                .Select(logicalTimer =>
                {
                    var snapshot = ZLinkSpotTimerRelocationCodec.Decode(
                        logicalTimer,
                        spotType);
                    if (!names.Add(snapshot.Timer.Name))
                        throw new InvalidDataException(
                            $"Duplicate logical timer '{snapshot.Timer.Name}'.");
                    return snapshot;
                })
                .ToArray();

            if (_restorePending)
            {
                RestoreConfiguredTimers(
                    snapshots,
                    stopToken,
                    dispatchAsync,
                    reportFailureAsync);
                _restorePending = false;
                _frozen = true;
                return;
            }
            if (_timers.Count != 0)
                throw new InvalidOperationException(
                    "Logical timers can only be restored into an empty registry.");

            foreach (var snapshot in snapshots)
            {
                var descriptor = ZLinkSpotDescriptorFactory.CreateTimerDescriptor(
                    snapshot.Timer.Name,
                    snapshot.Timer.Period,
                    snapshot.HandlerType,
                    snapshot.SpotType);
                var timer = new ZLinkTimer(
                    snapshot.Timer,
                    stopToken,
                    (tick, ct) => dispatchAsync(descriptor, tick, ct),
                    (tick, error, stopped, ct) =>
                        reportFailureAsync(descriptor, tick, error, stopped, ct),
                    () => ZLinkFlowContext.Enter(
                        null,
                        null,
                        flowCaptureEnabled(),
                        ZLinkFlowOrigin.Timer),
                    startFrozen: true,
                    scheduler: _scheduler);
                _timers.Add(new ZLinkSpotTimerRegistration(
                    timer,
                    snapshot.HandlerType,
                    snapshot.SpotType));
            }
            _frozen = true;
        }));
    }

    private void RestoreConfiguredTimers(
        IReadOnlyList<ZLinkSpotLogicalTimerSnapshot> snapshots,
        CancellationToken stopToken,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, CancellationToken, ValueTask<bool>> dispatchAsync,
        Func<ZLinkSpotTimerDescriptor, ZLinkTimerTick, Exception, bool, CancellationToken, ValueTask>
            reportFailureAsync)
    {
        var registrations = _timers.ToDictionary(
            static registration => registration.Timer.Snapshot().Name,
            StringComparer.Ordinal);
        foreach (var snapshot in snapshots)
        {
            if (registrations.TryGetValue(
                    snapshot.Timer.Name,
                    out var registration))
            {
                if (registration.HandlerType != snapshot.HandlerType
                    || registration.SpotType != snapshot.SpotType)
                    throw new InvalidDataException(
                        $"Logical timer '{snapshot.Timer.Name}' does not match its target registration.");
                registration.Timer.RestoreFrozen(snapshot.Timer);
                continue;
            }

            var descriptor = ZLinkSpotDescriptorFactory.CreateTimerDescriptor(
                snapshot.Timer.Name,
                snapshot.Timer.Period,
                snapshot.HandlerType,
                snapshot.SpotType);
            var timer = new ZLinkTimer(
                snapshot.Timer,
                stopToken,
                (tick, ct) => dispatchAsync(descriptor, tick, ct),
                (tick, error, stopped, ct) =>
                    reportFailureAsync(descriptor, tick, error, stopped, ct),
                () => ZLinkFlowContext.Enter(
                    null,
                    null,
                    flowCaptureEnabled(),
                    ZLinkFlowOrigin.Timer),
                startFrozen: true,
                scheduler: _scheduler);
            var restored = new ZLinkSpotTimerRegistration(
                timer,
                snapshot.HandlerType,
                snapshot.SpotType);
            if (!registrations.TryAdd(snapshot.Timer.Name, restored))
                throw new InvalidDataException(
                    $"Duplicate logical timer '{snapshot.Timer.Name}'.");
            _timers.Add(restored);
        }
    }

    internal void Resume()
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_frozen || _closed) return;
            _frozen = false;
            foreach (var registration in _timers)
                registration.Timer.Resume();
        }));
    }

    internal static ZLinkTimerOptions ValidateRegistration(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options)
    {
        if (string.IsNullOrWhiteSpace(name))
            throw new ZLinkConfigurationException("SPOT timer name must not be empty.");

        if (period <= TimeSpan.Zero)
            throw new ZLinkConfigurationException("SPOT timer period must be greater than zero.");

        var timerOptions = options ?? new ZLinkTimerOptions();
        if (!Enum.IsDefined(timerOptions.OverrunPolicy))
            throw new ZLinkConfigurationException("SPOT timer overrun policy is not supported.");

        if (timerOptions.OverrunPolicy == ZLinkTimerOverrunPolicy.CatchUpBounded
            && timerOptions.MaxCatchUpTicks <= 0)
            throw new ZLinkConfigurationException("SPOT timer MaxCatchUpTicks must be greater than zero.");

        return timerOptions;
    }

    private static async Task DisposeTimersAsync(
        IReadOnlyList<ZLinkSpotTimerRegistration> timers)
    {
        List<Exception>? failures = null;
        foreach (var registration in timers)
        {
            try
            {
                await registration.Timer.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        if (failures is [var failure])
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
    }

    private async Task CompleteFinalizationAsync(
        IReadOnlyList<ZLinkSpotTimerRegistration> timers,
        TaskCompletionSource completion)
    {
        List<Exception>? failures = null;
        try
        {
            await DisposeTimersAsync(timers).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            (failures ??= []).Add(exception);
        }

        if (_ownsScheduler)
        {
            try
            {
                await _scheduler.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        if (failures is null)
            completion.TrySetResult();
        else if (failures is [var failure])
            completion.TrySetException(failure);
        else
            completion.TrySetException(new AggregateException(failures));
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private sealed record ZLinkSpotTimerRegistration(
        ZLinkTimer Timer,
        Type HandlerType,
        Type SpotType);
}
