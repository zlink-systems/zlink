using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Diagnostics;

internal enum ZLinkRelocationUnitKind
{
    Actor,
    InstanceSpot,
    UserSpot
}

internal sealed class ZLinkRelocationInterruptionObserver
{
    private static readonly Action<
            ILogger, string, string?, bool, double, Exception?>
        TargetExceeded = LoggerMessage.Define<
            string, string?, bool, double>(
            LogLevel.Warning,
            new EventId(28001, "zlink.runtime.relocation.changed"),
            "zlink.runtime.relocation.changed unit_kind={UnitKind} "
            + "execution_mode={ExecutionMode} "
            + "interruption_target_exceeded={InterruptionTargetExceeded} "
            + "duration_seconds={DurationSeconds}");

    private readonly ILogger? _logger;
    private readonly TimeProvider _timeProvider;
    private readonly TimeSpan _target;

    internal ZLinkRelocationInterruptionObserver(
        ILoggerFactory? loggerFactory,
        TimeProvider? timeProvider = null,
        TimeSpan? target = null)
    {
        _logger = CreateLogger(loggerFactory);
        _timeProvider = timeProvider ?? TimeProvider.System;
        _target = target ?? TimeSpan.FromSeconds(1);
    }

    private static ILogger? CreateLogger(ILoggerFactory? loggerFactory)
    {
        try
        {
            return loggerFactory?.CreateLogger(
                "Zlink.Framework.Relocation");
        }
        catch
        {
            return null;
        }
    }

    internal ZLinkRelocationInterruptionOperation Start(
        ZLinkRelocationUnitKind unitKind,
        string? executionMode = null)
    {
        if (!ZLinkRuntimeMetrics.RelocationInterruptionEnabled
            && !WarningEnabled())
            return ZLinkRelocationInterruptionOperation.Disabled;

        return new ZLinkRelocationInterruptionOperation(
            this,
            _timeProvider.GetTimestamp(),
            UnitKind(unitKind),
            executionMode);
    }

    internal void Complete(
        long startedTimestamp,
        string unitKind,
        string? executionMode)
    {
        var duration = _timeProvider.GetElapsedTime(startedTimestamp);
        ZLinkRuntimeMetrics.RecordRelocationInterruption(
            duration,
            unitKind,
            executionMode);
        var logger = _logger;
        if (duration <= _target || logger is null || !WarningEnabled())
            return;

        try
        {
            TargetExceeded(
                logger,
                unitKind,
                executionMode,
                true,
                duration.TotalSeconds,
                null);
        }
        catch
        {
            // A monitoring provider must not change relocation completion.
        }
    }

    private bool WarningEnabled()
    {
        try
        {
            return _logger?.IsEnabled(LogLevel.Warning) == true;
        }
        catch
        {
            return false;
        }
    }

    private static string UnitKind(
        ZLinkRelocationUnitKind unitKind) =>
        unitKind switch
        {
            ZLinkRelocationUnitKind.Actor => "actor",
            ZLinkRelocationUnitKind.InstanceSpot => "instance_spot",
            ZLinkRelocationUnitKind.UserSpot => "user_spot",
            _ => throw new ArgumentOutOfRangeException(nameof(unitKind))
        };
}

internal sealed class ZLinkRelocationInterruptionOperation
{
    internal static readonly ZLinkRelocationInterruptionOperation Disabled =
        new(null, 0, "", null);

    private readonly ZLinkRelocationInterruptionObserver? _observer;
    private readonly long _startedTimestamp;
    private readonly string _unitKind;
    private readonly string? _executionMode;
    private int _completed;

    internal ZLinkRelocationInterruptionOperation(
        ZLinkRelocationInterruptionObserver? observer,
        long startedTimestamp,
        string unitKind,
        string? executionMode)
    {
        _observer = observer;
        _startedTimestamp = startedTimestamp;
        _unitKind = unitKind;
        _executionMode = executionMode;
    }

    internal bool Enabled => _observer is not null;

    internal void Complete()
    {
        if (_observer is null
            || Interlocked.Exchange(ref _completed, 1) != 0)
            return;
        _observer.Complete(
            _startedTimestamp,
            _unitKind,
            _executionMode);
    }
}
