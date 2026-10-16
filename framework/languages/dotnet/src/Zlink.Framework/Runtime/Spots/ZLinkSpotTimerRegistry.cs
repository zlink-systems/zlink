using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Timers;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkSpotLogicalTimerSnapshot(
    Type HandlerType,
    Type SpotType,
    ZLinkTimerLogicalSnapshot Timer
);

internal sealed class ZLinkSpotTimerRegistry(
    Func<bool> flowCaptureEnabled,
    bool restorePending = false,
    ZLinkTimerScheduler? scheduler = null
) : IAsyncDisposable
{
    private readonly ZLinkTimerScheduler _scheduler = scheduler ?? new();
    private readonly bool _ownsScheduler = scheduler is null;
    private readonly ZLinkStateLane _lane = new();
    private readonly List<ZLinkSpotTimerRegistration> _timers = [];
    private Task _finalization = Task.CompletedTask;
    private readonly List<ZLinkSpotTimerRegistration> _undisposedTimers = [];
    private bool _schedulerDisposed;
    private bool _closed;
    private bool _frozen = restorePending;
    private RelocationInitialization _relocationInitialization = restorePending
        ? RelocationInitialization.AwaitingSnapshot
        : RelocationInitialization.None;

    internal bool IsFrozen => AwaitStateLane(_lane.RunAsync(() => _frozen));

    public async ValueTask DisposeAsync()
    {
        var finalization = await _lane.RunAsync(GetOrStartFinalization).ConfigureAwait(false);
        await finalization.ConfigureAwait(false);
    }

    private Task GetOrStartFinalization()
    {
        if (!_closed)
        {
            _closed = true;
            _undisposedTimers.AddRange(_timers);
            _timers.Clear();
        }
        Task ContinueFinalization() =>
            _finalization
                .ContinueWith(
                    _ => ReleaseUndisposedAsync(),
                    CancellationToken.None,
                    TaskContinuationOptions.None,
                    TaskScheduler.Default
                )
                .Unwrap();
        return _finalization = ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(
            ContinueFinalization
        );
    }

    public ValueTask<IZLinkTimer> AddAsync(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options,
        Type handlerType,
        Type spotType,
        CancellationToken stopToken,
        Func<
            ZLinkSpotTimerDescriptor,
            ZLinkTimerTick,
            CancellationToken,
            ValueTask<bool>
        > dispatchAsync,
        Func<
            ZLinkSpotTimerDescriptor,
            ZLinkTimerTick,
            Exception,
            bool,
            CancellationToken,
            ValueTask
        > reportFailureAsync,
        CancellationToken cancellationToken
    )
    {
        var initializing = RelocatedInitializationScope.IsActiveFor(this);
        return _lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            cancellationToken.ThrowIfCancellationRequested();
            var initializingTarget =
                _relocationInitialization == RelocationInitialization.Restored && initializing;
            if (
                _frozen
                && _relocationInitialization != RelocationInitialization.AwaitingSnapshot
                && !initializingTarget
            )
                throw new ZLinkConfigurationException(
                    "SPOT timer registration is sealed for relocation."
                );

            var timerOptions = ValidateRegistration(name, period, options);

            if (initializingTarget)
            {
                var restored = _timers.Find(registration =>
                    registration.Restored
                    && !registration.Timer.IsDisposed
                    && registration.Timer.Snapshot().Name == name
                );
                if (restored is not null)
                {
                    restored.ValidateHandlerType(handlerType, spotType);
                    // Restored callbacks already dispatch through this target activation.
                    // Returning its handle preserves the logical cursor and pending tick.
                    return (IZLinkTimer)restored.Timer;
                }
            }

            var descriptor = ZLinkSpotDescriptorFactory.CreateTimerDescriptor(
                name,
                period,
                handlerType,
                spotType
            );
            var timer = new ZLinkTimer(
                new ZLinkTimerLogicalSnapshot(
                    name,
                    period,
                    timerOptions,
                    DateTimeOffset.UtcNow,
                    0,
                    0,
                    null,
                    null
                ),
                stopToken,
                (tick, ct) => dispatchAsync(descriptor, tick, ct),
                (tick, error, stopped, ct) =>
                    reportFailureAsync(descriptor, tick, error, stopped, ct),
                () =>
                    ZLinkFlowContext.Enter(null, null, flowCaptureEnabled(), ZLinkFlowOrigin.Timer),
                startFrozen: _frozen,
                scheduler: _scheduler
            );
            _timers.Add(new ZLinkSpotTimerRegistration(timer, handlerType, spotType));
            return (IZLinkTimer)timer;
        });
    }

    internal IReadOnlyList<ZLinkSpotLogicalTimerSnapshot> Freeze() =>
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                ObjectDisposedException.ThrowIf(_closed, this);
                _frozen = true;
                return _timers
                    .Where(static registration => !registration.Timer.IsDisposed)
                    .Select(static registration => new ZLinkSpotLogicalTimerSnapshot(
                        registration.HandlerType,
                        registration.SpotType,
                        registration.Timer.Freeze()
                    ))
                    .OrderBy(static snapshot => snapshot.Timer.Name, StringComparer.Ordinal)
                    .ToArray();
            })
        );

    internal IReadOnlyList<ZLinkRelocationLogicalTimer> FreezeRelocation()
    {
        return Freeze().Select(ZLinkSpotTimerRelocationCodec.Encode).ToArray();
    }

    internal async ValueTask<
        IReadOnlyList<ZLinkRelocationLogicalTimer>
    > SnapshotFrozenRelocationAfterDispatchesAsync(CancellationToken cancellationToken)
    {
        var registrations = await _lane
            .RunAsync(() =>
            {
                ObjectDisposedException.ThrowIf(_closed, this);
                if (!_frozen)
                    throw new InvalidOperationException(
                        "SPOT logical timers must be frozen before relocation snapshot."
                    );
                return _timers
                    .Where(static registration => !registration.Timer.IsDisposed)
                    .ToArray();
            })
            .ConfigureAwait(false);

        await Task.WhenAll(
                registrations.Select(static registration =>
                    registration.Timer.WaitForFrozenDispatchAsync()
                )
            )
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);

        return await _lane
            .RunAsync(() =>
            {
                ObjectDisposedException.ThrowIf(_closed, this);
                if (!_frozen)
                    throw new InvalidOperationException(
                        "SPOT logical timers resumed before relocation snapshot."
                    );
                return registrations
                    .Where(static registration => !registration.Timer.IsDisposed)
                    .Select(static registration => new ZLinkSpotLogicalTimerSnapshot(
                        registration.HandlerType,
                        registration.SpotType,
                        registration.Timer.Snapshot()
                    ))
                    .OrderBy(static snapshot => snapshot.Timer.Name, StringComparer.Ordinal)
                    .Select(ZLinkSpotTimerRelocationCodec.Encode)
                    .ToArray();
            })
            .ConfigureAwait(false);
    }

    internal void FreezeForApplicationAdmissionSeal()
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_closed || _frozen)
                    return;
                _frozen = true;
                foreach (var registration in _timers)
                {
                    if (!registration.Timer.IsDisposed)
                        _ = registration.Timer.Freeze();
                }
            })
        );
    }

    internal void RestoreRelocation(
        IReadOnlyList<ZLinkRelocationLogicalTimer> logicalTimers,
        CancellationToken stopToken,
        Func<
            ZLinkSpotTimerDescriptor,
            ZLinkTimerTick,
            CancellationToken,
            ValueTask<bool>
        > dispatchAsync,
        Func<
            ZLinkSpotTimerDescriptor,
            ZLinkTimerTick,
            Exception,
            bool,
            CancellationToken,
            ValueTask
        > reportFailureAsync
    ) =>
        RestoreRelocation(
            logicalTimers,
            typeof(object),
            stopToken,
            dispatchAsync,
            reportFailureAsync
        );

    internal void RestoreRelocation(
        IReadOnlyList<ZLinkRelocationLogicalTimer> logicalTimers,
        Type spotType,
        CancellationToken stopToken,
        Func<
            ZLinkSpotTimerDescriptor,
            ZLinkTimerTick,
            CancellationToken,
            ValueTask<bool>
        > dispatchAsync,
        Func<
            ZLinkSpotTimerDescriptor,
            ZLinkTimerTick,
            Exception,
            bool,
            CancellationToken,
            ValueTask
        > reportFailureAsync
    )
    {
        ArgumentNullException.ThrowIfNull(logicalTimers);
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                ObjectDisposedException.ThrowIf(_closed, this);
                var names = new HashSet<string>(StringComparer.Ordinal);
                var snapshots = logicalTimers
                    .Select(logicalTimer =>
                    {
                        var snapshot = ZLinkSpotTimerRelocationCodec.Decode(logicalTimer, spotType);
                        if (!names.Add(snapshot.Timer.Name))
                            throw new InvalidDataException(
                                $"Duplicate logical timer '{snapshot.Timer.Name}'."
                            );
                        return snapshot;
                    })
                    .ToArray();

                if (_timers.Count != 0)
                    throw new InvalidOperationException(
                        "Logical timers can only be restored into an empty registry."
                    );

                foreach (var snapshot in snapshots)
                {
                    var descriptor = ZLinkSpotDescriptorFactory.CreateTimerDescriptor(
                        snapshot.Timer.Name,
                        snapshot.Timer.Period,
                        snapshot.HandlerType,
                        snapshot.SpotType
                    );
                    var timer = new ZLinkTimer(
                        snapshot.Timer,
                        stopToken,
                        (tick, ct) => dispatchAsync(descriptor, tick, ct),
                        (tick, error, stopped, ct) =>
                            reportFailureAsync(descriptor, tick, error, stopped, ct),
                        () =>
                            ZLinkFlowContext.Enter(
                                null,
                                null,
                                flowCaptureEnabled(),
                                ZLinkFlowOrigin.Timer
                            ),
                        startFrozen: true,
                        scheduler: _scheduler
                    );
                    _timers.Add(
                        new ZLinkSpotTimerRegistration(
                            timer,
                            snapshot.HandlerType,
                            snapshot.SpotType
                        )
                        {
                            Restored = true,
                        }
                    );
                }
                _relocationInitialization = RelocationInitialization.Restored;
                _frozen = true;
            })
        );
    }

    internal IDisposable? EnterRelocatedInitialization() =>
        AwaitStateLane(
            _lane.RunAsync(() =>
                _relocationInitialization == RelocationInitialization.Restored && _frozen
            )
        )
            ? new RelocatedInitializationScope(this)
            : null;

    internal void Resume()
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (!_frozen || _closed)
                    return;
                _frozen = false;
                _relocationInitialization = RelocationInitialization.None;
                foreach (var registration in _timers)
                {
                    registration.Restored = false;
                    registration.Timer.Resume();
                }
            })
        );
    }

    internal static ZLinkTimerOptions ValidateRegistration(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options
    )
    {
        if (string.IsNullOrWhiteSpace(name))
            throw new ZLinkConfigurationException("SPOT timer name must not be empty.");

        if (period <= TimeSpan.Zero)
            throw new ZLinkConfigurationException("SPOT timer period must be greater than zero.");

        var timerOptions = options ?? new ZLinkTimerOptions();
        if (!Enum.IsDefined(timerOptions.OverrunPolicy))
            throw new ZLinkConfigurationException("SPOT timer overrun policy is not supported.");

        if (
            timerOptions.OverrunPolicy == ZLinkTimerOverrunPolicy.CatchUpBounded
            && timerOptions.MaxCatchUpTicks <= 0
        )
            throw new ZLinkConfigurationException(
                "SPOT timer MaxCatchUpTicks must be greater than zero."
            );

        return timerOptions;
    }

    // Timer registrations whose disposal failed stay owned; the next
    // DisposeAsync call retries only those (spec 06-spot-address-messaging §7
    // step 3 resume). A failed release is never cached as the terminal result.
    private async Task ReleaseUndisposedAsync()
    {
        List<Exception>? failures = null;
        var remaining = new List<ZLinkSpotTimerRegistration>();
        foreach (var registration in _undisposedTimers)
            try
            {
                await registration.Timer.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                remaining.Add(registration);
                (failures ??= []).Add(exception);
            }
        _undisposedTimers.Clear();
        _undisposedTimers.AddRange(remaining);

        if (_ownsScheduler && !_schedulerDisposed)
        {
            try
            {
                await _scheduler.DisposeAsync().ConfigureAwait(false);
                _schedulerDisposed = true;
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        if (failures is [var failure])
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
        if (failures is { Count: > 1 })
            throw new AggregateException(failures);
    }

    private sealed record ZLinkSpotTimerRegistration(
        ZLinkTimer Timer,
        Type HandlerType,
        Type SpotType
    )
    {
        internal bool Restored { get; set; }

        internal void ValidateHandlerType(Type handlerType, Type spotType)
        {
            if (HandlerType != handlerType || SpotType != spotType)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Restored timer '{Timer.Snapshot().Name}' handler type does not match its target registration."
                );
        }
    }

    private enum RelocationInitialization
    {
        None,
        AwaitingSnapshot,
        Restored,
    }

    private sealed class RelocatedInitializationScope : IDisposable
    {
        private static readonly AsyncLocal<RelocatedInitializationScope?> Current = new();
        private readonly ZLinkSpotTimerRegistry _registry;
        private readonly RelocatedInitializationScope? _previous;
        private int _active = 1;

        internal RelocatedInitializationScope(ZLinkSpotTimerRegistry registry)
        {
            _registry = registry;
            _previous = Current.Value;
            Current.Value = this;
        }

        internal static bool IsActiveFor(ZLinkSpotTimerRegistry registry) =>
            Current.Value is { } scope
            && Volatile.Read(ref scope._active) != 0
            && ReferenceEquals(scope._registry, registry);

        public void Dispose()
        {
            Interlocked.Exchange(ref _active, 0);
            Current.Value = _previous;
        }
    }
}
