using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class MaintenanceRuntimeTests
{
    [Fact]
    public async Task Planned_maintenance_relocates_without_stopping_the_host()
    {
        using var fixture = Create(sourceApplicationVersion: 7);
        fixture.Runtime.MarkServing();
        fixture.Executor.Complete.TrySetResult(null);

        var result = await fixture.Runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
        });

        Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, result.Outcome);
        Assert.Equal(7, result.TargetApplicationVersion);
        Assert.Equal(ZLinkFrameworkRuntimeState.Relocated, fixture.Runtime.Status.State);
        Assert.Null(fixture.Runtime.Status.TerminationResult);
        Assert.Equal(ZLinkFrameworkLifecycleIntent.Relocate, fixture.Executor.Intent);
    }

    [Fact]
    public async Task Shutdown_is_a_separate_operation_after_relocation()
    {
        using var fixture = Create(sourceApplicationVersion: 7);
        fixture.Runtime.MarkServing();
        fixture.Executor.Complete.TrySetResult(null);
        await fixture.Runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
        });

        var result = await fixture.Runtime.ShutdownAsync();

        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, result.Outcome);
        Assert.Equal(ZLinkFrameworkRuntimeState.Stopped, fixture.Runtime.Status.State);
        Assert.Equal(2, fixture.Executor.ExecuteCount);
        Assert.Equal(ZLinkFrameworkLifecycleIntent.Shutdown, fixture.Executor.Intent);
    }

    [Fact]
    public async Task Rolling_update_requires_a_newer_exact_target_version()
    {
        using var fixture = Create(sourceApplicationVersion: 7);
        fixture.Runtime.MarkServing();

        await Assert.ThrowsAsync<ArgumentException>(() =>
            fixture.Runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
            {
                Mode = ZLinkFrameworkRelocationMode.RollingUpdate,
                TargetApplicationVersion = 7
            }).AsTask());
        await Assert.ThrowsAsync<ArgumentException>(() =>
            fixture.Runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
            {
                Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                TargetApplicationVersion = 8
            }).AsTask());
    }

    [Fact]
    public async Task Preflight_blocker_keeps_the_host_serving()
    {
        using var fixture = Create(
            static (_, _, _) => ValueTask.FromResult<ZLinkFrameworkRelocationReason?>(
                ZLinkFrameworkRelocationReason.TargetUnavailable));
        fixture.Runtime.MarkServing();

        var result = await fixture.Runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
        });

        Assert.Equal(ZLinkFrameworkRelocationOutcome.Blocked, result.Outcome);
        Assert.Equal(ZLinkFrameworkRelocationReason.TargetUnavailable, result.Reason);
        Assert.Equal(ZLinkFrameworkRuntimeState.Serving, fixture.Runtime.Status.State);
        Assert.Null(fixture.Runtime.Status.RelocationResult);
    }

    [Fact]
    public async Task Target_wait_deadline_preserves_target_unavailable_reason()
    {
        using var cancellation = new CancellationTokenSource();
        var wait = ZLinkFrameworkRuntime.WaitForTargetAvailabilityAsync(
                TimeSpan.FromHours(1),
                cancellation.Token)
            .AsTask();

        cancellation.Cancel();

        Assert.Equal(
            ZLinkFrameworkRelocationReason.TargetUnavailable,
            await wait);
    }

    [Fact]
    public async Task Partial_retiring_publication_failure_keeps_the_host_fail_closed()
    {
        var executor = new MaintenanceExecutor();
        using var drain = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);
        using var runtime = new ZLinkFrameworkMaintenanceRuntime(
            drain,
            new ZLinkFrameworkHostLifecycleState(),
            static (_, _, _) => ValueTask.FromResult<ZLinkFrameworkRelocationReason?>(null),
            static _ => ValueTask.FromException<bool>(
                new ZLinkRetiringPublicationRollbackException(
                    [new InvalidOperationException("descriptor rollback failed")])),
            sourceApplicationVersion: 7);
        runtime.MarkServing();

        var result = await runtime.RelocateAsync(
            new ZLinkFrameworkRelocationOptions
            {
                Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
            });

        Assert.Equal(ZLinkFrameworkRelocationReason.RelocationFailed, result.Reason);
        Assert.Equal(ZLinkFrameworkRuntimeState.Stopped, runtime.Status.State);
        Assert.False(runtime.Status.AcceptingWork);
        Assert.Equal(
            ZLinkFrameworkTerminationOutcome.ForceStopped,
            runtime.Status.TerminationResult?.Outcome);
        Assert.Equal(
            ZLinkFrameworkTerminationReason.TeardownFailed,
            runtime.Status.TerminationResult?.Reason);
    }

    [Theory]
    [InlineData(ZLinkFrameworkRelocationMode.PlannedMaintenance, null, 7L)]
    [InlineData(ZLinkFrameworkRelocationMode.RollingUpdate, 9L, 9L)]
    public async Task Preflight_receives_mode_and_exact_effective_target_version(
        ZLinkFrameworkRelocationMode mode,
        long? requestedVersion,
        long expectedVersion)
    {
        ZLinkFrameworkRelocationMode? observedMode = null;
        long? observedVersion = null;
        using var fixture = Create(
            (candidateMode, candidateVersion, _) =>
            {
                observedMode = candidateMode;
                observedVersion = candidateVersion;
                return ValueTask.FromResult<ZLinkFrameworkRelocationReason?>(
                    ZLinkFrameworkRelocationReason.TargetUnavailable);
            },
            sourceApplicationVersion: 7);
        fixture.Runtime.MarkServing();

        var result = await fixture.Runtime.RelocateAsync(
            new ZLinkFrameworkRelocationOptions
            {
                Mode = mode,
                TargetApplicationVersion = requestedVersion
            });

        Assert.Equal(mode, observedMode);
        Assert.Equal(expectedVersion, observedVersion);
        Assert.Equal(ZLinkFrameworkRelocationReason.TargetUnavailable, result.Reason);
    }

    [Theory]
    [InlineData(ZLinkFrameworkRelocationMode.PlannedMaintenance, 7, 6, false)]
    [InlineData(ZLinkFrameworkRelocationMode.PlannedMaintenance, 7, 7, true)]
    [InlineData(ZLinkFrameworkRelocationMode.PlannedMaintenance, 7, 8, false)]
    [InlineData(ZLinkFrameworkRelocationMode.RollingUpdate, 9, 8, false)]
    [InlineData(ZLinkFrameworkRelocationMode.RollingUpdate, 9, 9, true)]
    [InlineData(ZLinkFrameworkRelocationMode.RollingUpdate, 9, 10, false)]
    public void Target_selection_never_uses_lower_or_higher_version_fallback(
        ZLinkFrameworkRelocationMode mode,
        long exactTargetVersion,
        long candidateVersion,
        bool expected)
    {
        var selection = new ZLinkRelocationTargetSelection(
            mode,
            exactTargetVersion);

        Assert.Equal(expected, selection.Matches(candidateVersion));
    }

    [Fact]
    public async Task Relocate_before_serving_is_blocked_but_shutdown_is_allowed()
    {
        using var fixture = Create();
        fixture.Executor.Complete.TrySetResult(null);

        var blocked = await fixture.Runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
        });
        var stopped = await fixture.Runtime.ShutdownAsync();

        Assert.Equal(ZLinkFrameworkRelocationReason.RuntimeNotReady, blocked.Reason);
        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, stopped.Outcome);
        Assert.Equal(ZLinkFrameworkRuntimeState.Stopped, fixture.Runtime.Status.State);
    }

    [Fact]
    public async Task Observe_reports_the_latest_relocated_status()
    {
        using var fixture = Create();
        fixture.Runtime.MarkServing();
        var observed = new List<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>();
        using var stop = new CancellationTokenSource();
        var observer = Task.Run(async () =>
        {
            await foreach (var status in fixture.Runtime.ObserveAsync(stop.Token))
            {
                observed.Add(status);
                if (status.Status.State == ZLinkFrameworkRuntimeState.Relocated)
                    break;
            }
        });

        fixture.Executor.Complete.TrySetResult(null);
        await fixture.Runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
        });
        await observer.WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Contains(observed, status =>
            status.Status.State == ZLinkFrameworkRuntimeState.Relocated);
    }

    [Fact]
    public async Task Observe_preserves_transient_blocked_relocation_result()
    {
        using var fixture = Create(
            static (_, _, _) => ValueTask.FromResult<ZLinkFrameworkRelocationReason?>(
                ZLinkFrameworkRelocationReason.TargetUnavailable));
        fixture.Runtime.MarkServing();
        await using var observer = fixture.Runtime.ObserveAsync().GetAsyncEnumerator();

        Assert.True(await observer.MoveNextAsync());
        var relocation = fixture.Runtime.RelocateAsync(
            new ZLinkFrameworkRelocationOptions
            {
                Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
            });

        ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus> terminal;
        do
        {
            Assert.True(await observer.MoveNextAsync());
            terminal = observer.Current;
        }
        while (terminal.Status.RelocationResult is null);

        var result = await relocation;
        Assert.Equal(ZLinkFrameworkRelocationReason.TargetUnavailable, result.Reason);
        Assert.Equal(ZLinkFrameworkRuntimeState.Serving, fixture.Runtime.Status.State);
        Assert.Null(fixture.Runtime.Status.RelocationResult);
        Assert.Equal(result, terminal.Status.RelocationResult);
        Assert.True(terminal.Status.Sequence > 0);
    }

    [Fact]
    public async Task Relocated_runtime_replays_the_first_terminal_result_for_every_intent()
    {
        using var fixture = Create(sourceApplicationVersion: 7);
        fixture.Runtime.MarkServing();
        fixture.Executor.Complete.TrySetResult(null);
        var rollingOptions = new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.RollingUpdate,
            TargetApplicationVersion = 8
        };

        var first = await fixture.Runtime.RelocateAsync(rollingOptions);
        var replay = await fixture.Runtime.RelocateAsync(rollingOptions);
        var planned = await fixture.Runtime.RelocateAsync(
            new ZLinkFrameworkRelocationOptions
            {
                Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance
            });

        Assert.Equal(first, replay);
        Assert.Equal(first, planned);
        Assert.Equal(ZLinkFrameworkRelocationMode.RollingUpdate, planned.Mode);
        Assert.Equal(8, planned.TargetApplicationVersion);
        Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, planned.Outcome);
        Assert.Equal(1, fixture.Executor.ExecuteCount);
    }

    [Fact]
    public async Task Shutdown_cancels_only_the_shared_relocation_operation_not_its_waiters()
    {
        var preflightEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var fixture = Create(async (_, _, cancellationToken) =>
        {
            preflightEntered.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return null;
        });
        fixture.Runtime.MarkServing();
        fixture.Executor.Complete.TrySetResult(null);
        var options = new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
            Deadline = TimeSpan.FromSeconds(30)
        };

        var primary = fixture.Runtime.RelocateAsync(options).AsTask();
        await preflightEntered.Task.WaitAsync(TimeSpan.FromSeconds(2));
        using var cancelledWaiter = new CancellationTokenSource();
        var waiter = fixture.Runtime.RelocateAsync(
            options,
            cancelledWaiter.Token).AsTask();
        cancelledWaiter.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => waiter);

        var shutdown = fixture.Runtime.ShutdownAsync().AsTask();
        var relocationResult = await primary;
        var shutdownResult = await shutdown;
        var relocationReplay = await fixture.Runtime.RelocateAsync(options);
        var shutdownReplay = await fixture.Runtime.ShutdownAsync();

        Assert.Equal(
            ZLinkFrameworkRelocationReason.ShutdownRequested,
            relocationResult.Reason);
        Assert.Equal(ZLinkFrameworkRelocationOutcome.Blocked, relocationResult.Outcome);
        Assert.Equal(relocationResult, relocationReplay);
        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, shutdownResult.Outcome);
        Assert.Equal(shutdownResult, shutdownReplay);
        Assert.Equal(1, fixture.Executor.ExecuteCount);
    }

    [Fact]
    public async Task Shutdown_after_relocating_seals_immediately_and_starts_no_next_unit()
    {
        using var fixture = Create();
        fixture.Runtime.MarkServing();
        var options = new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
            Deadline = TimeSpan.FromSeconds(30)
        };

        var relocation = fixture.Runtime.RelocateAsync(options).AsTask();
        await fixture.Executor.Started.Task.WaitAsync(TimeSpan.FromSeconds(2));
        var shutdown = fixture.Runtime
            .ShutdownAsync(TimeSpan.FromSeconds(2))
            .AsTask();

        Assert.Equal(
            ZLinkFrameworkRuntimeState.Draining,
            fixture.Runtime.Status.State);
        Assert.Equal(1, fixture.Executor.ShutdownRequestCount);

        var relocationResult = await relocation;
        Assert.Equal(
            ZLinkFrameworkRelocationOutcome.Blocked,
            relocationResult.Outcome);
        Assert.Equal(
            ZLinkFrameworkRelocationReason.ShutdownRequested,
            relocationResult.Reason);

        fixture.Executor.Complete.TrySetResult(null);
        var shutdownResult = await shutdown;
        Assert.Equal(
            ZLinkFrameworkTerminationOutcome.Stopped,
            shutdownResult.Outcome);
        Assert.Equal(2, fixture.Executor.ExecuteCount);
    }

    [Fact]
    public async Task Shutdown_force_stop_deadline_remains_public_deadline_exceeded()
    {
        using var fixture = Create();
        fixture.Runtime.MarkServing();
        fixture.Executor.CancelForceStop = true;

        var result = await fixture.Runtime.ShutdownAsync(
            TimeSpan.FromMilliseconds(20));

        Assert.Equal(
            ZLinkFrameworkTerminationOutcome.ForceStopped,
            result.Outcome);
        Assert.Equal(
            ZLinkFrameworkTerminationReason.DeadlineExceeded,
            result.Reason);
    }

    [Fact]
    public void Status_Reads_Current_Inbound_Dispatch_Snapshot()
    {
        var inbound = new ZLinkInboundDispatchStatus(
            4096,
            1024,
            768,
            256,
            true,
            4,
            64);
        using var fixture = Create(inboundDispatchSnapshot: () => inbound);

        Assert.Equal(inbound, fixture.Runtime.Status.InboundDispatch);

        inbound = inbound with
        {
            PendingPayloadBytes = 0,
            QueuedPayloadBytes = 0,
            ActivePayloadBytes = 0,
            ApplicationReceivePaused = false
        };
        Assert.Equal(inbound, fixture.Runtime.Status.InboundDispatch);
    }

    private static Fixture Create(
        Func<
            ZLinkFrameworkRelocationMode,
            long,
            CancellationToken,
            ValueTask<ZLinkFrameworkRelocationReason?>>? preflight = null,
        long sourceApplicationVersion = 0,
        Func<ZLinkInboundDispatchStatus>? inboundDispatchSnapshot = null)
    {
        var executor = new MaintenanceExecutor();
        var drain = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);
        var runtime = new ZLinkFrameworkMaintenanceRuntime(
            drain,
            new ZLinkFrameworkHostLifecycleState(),
            preflight ?? (static (_, _, _) =>
                ValueTask.FromResult<ZLinkFrameworkRelocationReason?>(null)),
            static _ => ValueTask.FromResult(true),
            sourceApplicationVersion,
            inboundDispatchSnapshot);
        return new Fixture(runtime, drain, executor);
    }

    private sealed record Fixture(
        ZLinkFrameworkMaintenanceRuntime Runtime,
        ZLinkDrainCoordinator Drain,
        MaintenanceExecutor Executor) : IDisposable
    {
        public void Dispose()
        {
            Runtime.Dispose();
            Drain.Dispose();
        }
    }

    private sealed class MaintenanceExecutor : IZLinkDrainExecutor
    {
        public TaskCompletionSource<ZLinkDrainForceReason?> Complete { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        private TaskCompletionSource ShutdownRequested { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int ExecuteCount { get; private set; }

        public int ShutdownRequestCount { get; private set; }

        public ZLinkFrameworkLifecycleIntent? Intent { get; private set; }

        public bool CancelForceStop { get; set; }

        public void RequestShutdown(TimeSpan deadline)
        {
            _ = deadline;
            ShutdownRequestCount++;
            ShutdownRequested.TrySetResult();
        }

        public ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
            TimeSpan deadline,
            CancellationToken deadlineToken) =>
            ExecuteAsync(ZLinkFrameworkLifecycleIntent.Shutdown, deadline, deadlineToken);

        public async ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
            ZLinkFrameworkLifecycleIntent intent,
            TimeSpan deadline,
            CancellationToken deadlineToken)
        {
            _ = deadline;
            Intent = intent;
            ExecuteCount++;
            Started.TrySetResult();
            if (intent == ZLinkFrameworkLifecycleIntent.Relocate)
            {
                var first = await Task.WhenAny(
                        Complete.Task,
                        ShutdownRequested.Task)
                    .ConfigureAwait(false);
                if (ReferenceEquals(first, ShutdownRequested.Task))
                    throw new ZLinkDrainBlockedException(
                        ZLinkFrameworkRelocationReason.ShutdownRequested);
            }
            return await Complete.Task.WaitAsync(deadlineToken).ConfigureAwait(false);
        }

        public async ValueTask ForceStopAsync(
            ZLinkDrainForceReason reason,
            CancellationToken cancellationToken)
        {
            if (CancelForceStop)
                await Task.Delay(
                        Timeout.InfiniteTimeSpan,
                        cancellationToken)
                    .ConfigureAwait(false);
        }
    }
}
