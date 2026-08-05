using Zlink.Framework.Runtime.Timers;
using Zlink.Framework.Runtime.Locations;

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
    private readonly object _lifecycleGate = new();
    private readonly List<ZLinkSpotTimerRegistration> _timers = [];
    private Task? _finalization;
    private bool _closed;
    private bool _frozen = restorePending;
    private bool _restorePending = restorePending;

    internal bool IsFrozen
    {
        get
        {
            lock (_lifecycleGate)
                return _frozen;
        }
    }

    public ValueTask DisposeAsync()
    {
        TaskCompletionSource completion;
        ZLinkSpotTimerRegistration[] timers;
        lock (_lifecycleGate)
        {
            if (_finalization is not null) return new ValueTask(_finalization);

            _closed = true;
            timers = _timers.ToArray();
            _timers.Clear();
            completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _finalization = completion.Task;
        }

        _ = CompleteFinalizationAsync(timers, completion);
        return new ValueTask(completion.Task);
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
        lock (_lifecycleGate)
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
            return ValueTask.FromResult<IZLinkTimer>(timer);
        }
    }

    internal IReadOnlyList<ZLinkSpotLogicalTimerSnapshot> Freeze()
    {
        lock (_lifecycleGate)
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
        }
    }

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
        ZLinkSpotTimerRegistration[] registrations;
        lock (_lifecycleGate)
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            if (!_frozen)
                throw new InvalidOperationException(
                    "SPOT logical timers must be frozen before relocation snapshot.");
            registrations = _timers
                .Where(static registration => !registration.Timer.IsDisposed)
                .ToArray();
        }

        await Task.WhenAll(registrations.Select(
                static registration =>
                    registration.Timer.WaitForFrozenDispatchAsync()))
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);

        lock (_lifecycleGate)
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
        }
    }

    internal void FreezeForApplicationAdmissionSeal()
    {
        lock (_lifecycleGate)
        {
            if (_closed || _frozen) return;
            _frozen = true;
            foreach (var registration in _timers)
            {
                if (!registration.Timer.IsDisposed)
                    _ = registration.Timer.Freeze();
            }
        }
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
        lock (_lifecycleGate)
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
        }
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
        lock (_lifecycleGate)
        {
            if (!_frozen || _closed) return;
            _frozen = false;
            foreach (var registration in _timers)
                registration.Timer.Resume();
        }
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

    private sealed record ZLinkSpotTimerRegistration(
        ZLinkTimer Timer,
        Type HandlerType,
        Type SpotType);
}
