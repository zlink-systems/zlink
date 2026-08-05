using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Diagnostics.HealthChecks;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class DrainCoordinatorTests
{
    [Fact]
    public void Propagation_Bound_Is_Polling_Plus_Five_Seconds_And_One_Hundred_Milliseconds()
    {
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.FromSeconds(2)
        };

        Assert.Equal(
            TimeSpan.FromMilliseconds(7_100),
            ZLinkFrameworkDrainExecutor.CalculatePropagationDelay(options));
    }

    [Fact]
    public async Task Drain_Executor_Seals_Before_Publishing_And_Waits_Accepted_Work_Before_Handoff()
    {
        var probe = new DrainExecutionProbe();
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions
            {
                // The fixed 5s read bound plus 100ms jitter leaves a 1ms
                // propagation delay while preserving the production formula.
                PollingInterval = TimeSpan.FromMilliseconds(-5_099)
            });

        var reason = await executor.ExecuteAsync(
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        Assert.Null(reason);
        Assert.Equal(
            new[]
            {
                "seal-admission",
                "marker",
                "quiesce-serving-channels",
                "wait-accepted",
                "wait-accepted-handoffs",
                "drain-actors",
                "drain-spots",
                "drain-sessions",
                "freeze-owner-writes",
                "stop-auto-connect",
                "cleanup-owner",
                "stop-runtime",
                "stop-location"
            },
            probe.Events);
    }

    [Fact]
    public async Task Retire_Uses_Spot_Relocation_Instead_Of_Explicit_Close()
    {
        var probe = new DrainExecutionProbe();
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions
            {
                PollingInterval = TimeSpan.FromMilliseconds(-5_099)
            });

        var reason = await executor.ExecuteAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        Assert.Null(reason);
        Assert.True(probe.LastSpotDrainWasRelocation);
    }

    [Fact]
    public async Task Relocation_workload_coordinator_moves_shells_then_actors_then_aggregates()
    {
        var events = new List<string>();
        var coordinator = new ZLinkRelocationWorkloadCoordinator(
            (phase, _) =>
            {
                events.Add(phase == ZLinkSpotRelocationPhase.PerActorShells
                    ? "shells"
                    : "aggregates");
                return ValueTask.FromResult(
                    new ZLinkSpotDrainResult(
                        true,
                        phase == ZLinkSpotRelocationPhase.PerActorShells
                            ? 1UL
                            : 3UL,
                        null,
                        ZLinkRelocationCommitKnowledge.Committed,
                        true));
            },
            (_, _) =>
            {
                events.Add("actors");
                return ValueTask.FromResult(
                    new ZLinkActorDrainResult(
                        true,
                        null,
                        2,
                        ZLinkRelocationCommitKnowledge.Committed,
                        true));
            });

        var result = await coordinator.DrainAsync(
            new ZLinkRelocationWorkloadDrainControl(
                static () => false,
                CancellationToken.None));

        Assert.True(result.Completed);
        Assert.Null(result.TerminalReason);
        Assert.Equal<ulong>(6, result.CommittedUnitCount);
        Assert.Equal(
            ZLinkRelocationCommitKnowledge.Committed,
            result.CommitKnowledge);
        Assert.True(result.SourceTerminalized);
        Assert.Equal(new[] { "shells", "actors", "aggregates" }, events);
    }

    [Fact]
    public async Task Relocation_workload_coordinator_preserves_aggregate_terminal_reason()
    {
        var coordinator = new ZLinkRelocationWorkloadCoordinator(
            (phase, _) => ValueTask.FromResult(
                phase == ZLinkSpotRelocationPhase.PerActorShells
                    ? new ZLinkSpotDrainResult(
                        true,
                        1,
                        null,
                        ZLinkRelocationCommitKnowledge.Committed,
                        true)
                    : new ZLinkSpotDrainResult(
                        false,
                        0,
                        ZLinkFrameworkRelocationReason.RelocationFailed,
                        ZLinkRelocationCommitKnowledge.NotCommitted,
                        true)),
            (_, _) => ValueTask.FromResult(
                new ZLinkActorDrainResult(
                    true,
                    null,
                    1,
                    ZLinkRelocationCommitKnowledge.Committed,
                    true)));

        var result = await coordinator.DrainAsync(
            new ZLinkRelocationWorkloadDrainControl(
                static () => false,
                CancellationToken.None));

        Assert.False(result.Completed);
        Assert.Equal(
            ZLinkFrameworkRelocationReason.RelocationFailed,
            result.TerminalReason);
        Assert.Equal<ulong>(2, result.CommittedUnitCount);
        Assert.Equal(
            ZLinkRelocationCommitKnowledge.Committed,
            result.CommitKnowledge);
        Assert.True(result.SourceTerminalized);
    }

    [Fact]
    public async Task Shutdown_signal_between_phases_starts_no_new_relocation_unit()
    {
        var events = new List<string>();
        var stopRequested = false;
        var coordinator = new ZLinkRelocationWorkloadCoordinator(
            (phase, _) =>
            {
                events.Add(phase == ZLinkSpotRelocationPhase.PerActorShells
                    ? "shells"
                    : "aggregates");
                stopRequested = true;
                return ValueTask.FromResult(
                    new ZLinkSpotDrainResult(true, 1));
            },
            (_, _) =>
            {
                events.Add("actors");
                return ValueTask.FromResult(
                    new ZLinkActorDrainResult(true, null, 1));
            });

        var result = await coordinator.DrainAsync(
            new ZLinkRelocationWorkloadDrainControl(
                () => stopRequested,
                CancellationToken.None));

        Assert.False(result.Completed);
        Assert.Equal<ulong>(1, result.CommittedUnitCount);
        Assert.Equal(new[] { "shells" }, events);
    }

    [Fact]
    public async Task GenerationExhaustionStopsSpotDrainWithoutPollingRetry()
    {
        var probe = new DrainExecutionProbe();
        var attempts = 0;
        var operations = probe.Operations with
        {
            DrainRelocationWorkloads = _ =>
            {
                Interlocked.Increment(ref attempts);
                return ValueTask.FromException<
                    ZLinkRelocationWorkloadDrainResult>(
                    new ZLinkAuthorityGenerationExhaustedException(
                        "testing drain classification"));
            }
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions
            {
                PollingInterval = TimeSpan.FromMilliseconds(-5_099)
            });

        var blocked = await Assert.ThrowsAsync<ZLinkDrainBlockedException>(async () =>
            await executor.ExecuteAsync(
                ZLinkFrameworkLifecycleIntent.Relocate,
                TimeSpan.FromSeconds(1),
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkRelocationReason.RelocationFailed, blocked.Reason);
        Assert.Equal(1, attempts);
    }

    [Fact]
    public async Task RetireFailureBeforeFirstCommitRestoresServingAndReturnsBlocked()
    {
        var probe = new DrainExecutionProbe();
        var admission = new ZLinkDrainAdmissionGate();
        admission.BeginDrain();
        var operations = probe.Operations with
        {
            CaptureRelocationFence = () => new ZLinkRelocationAdmissionFence(1),
            ReopenRelocationAdmissions = (_, _) =>
            {
                probe.Events.Add("reopen-admission");
                admission.Reset();
                return true;
            },
            DrainRelocationWorkloads = _ => ValueTask.FromResult(
                new ZLinkRelocationWorkloadDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    0))
        };
        using var coordinator = new ZLinkDrainCoordinator(
            admission,
            new ZLinkFrameworkDrainExecutor(
                operations,
                new ZLinkLocationOptions()));

        var result = await coordinator.DrainAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1));

        var blocked = Assert.IsType<DrainBlocked>(result);
        Assert.Equal(ZLinkFrameworkRelocationReason.RelocationFailed, blocked.Reason);
        Assert.True(coordinator.IsReady);
        Assert.Contains("reopen-admission", probe.Events);
    }

    [Fact]
    public async Task Executor_does_not_restore_serving_for_a_stale_relocation_fence()
    {
        var probe = new DrainExecutionProbe();
        var operations = probe.Operations with
        {
            RestoreServing = _ =>
            {
                probe.Events.Add("restore-serving");
                return ValueTask.FromResult(true);
            },
            CaptureRelocationFence = static () =>
                new ZLinkRelocationAdmissionFence(2),
            AcquireRelocationRollback = static _ => null,
            DrainRelocationWorkloads = _ => ValueTask.FromResult(
                new ZLinkRelocationWorkloadDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    0))
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var forceReason = await executor.ExecuteAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        Assert.Equal(ZLinkDrainForceReason.TeardownFailed, forceReason);
        Assert.DoesNotContain("restore-serving", probe.Events);
    }

    [Fact]
    public async Task Executor_does_not_restore_serving_after_rollback_lease_is_invalidated()
    {
        var probe = new DrainExecutionProbe();
        var operations = probe.Operations with
        {
            RestoreServing = _ =>
            {
                probe.Events.Add("restore-serving");
                return ValueTask.FromResult(true);
            },
            CaptureRelocationFence = static () =>
                new ZLinkRelocationAdmissionFence(3),
            AcquireRelocationRollback = static _ =>
            {
                var lease = new ZLinkRelocationRollbackLease();
                lease.Invalidate();
                return lease;
            },
            DrainRelocationWorkloads = _ => ValueTask.FromResult(
                new ZLinkRelocationWorkloadDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    0))
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var forceReason = await executor.ExecuteAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        Assert.Equal(ZLinkDrainForceReason.TeardownFailed, forceReason);
        Assert.DoesNotContain("restore-serving", probe.Events);
    }

    [Fact]
    public async Task Full_rollback_shutdown_takeover_returns_shutdown_blocked()
    {
        var probe = new DrainExecutionProbe();
        ZLinkFrameworkDrainExecutor? executor = null;
        ZLinkRelocationRollbackLease? lease = null;
        var operations = probe.Operations with
        {
            AcquireRelocationRollback = _ =>
            {
                lease = new ZLinkRelocationRollbackLease();
                return lease;
            },
            RestoreServing = _ =>
            {
                lease!.Invalidate();
                executor!.RequestShutdown(TimeSpan.FromSeconds(1));
                return ValueTask.FromResult(true);
            },
            DrainRelocationWorkloads = _ => ValueTask.FromResult(
                new ZLinkRelocationWorkloadDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    0))
        };
        executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var result = await executor.ExecuteWithProgressAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1),
            null,
            CancellationToken.None);

        Assert.Equal(
            ZLinkDrainExecutionDisposition.Blocked,
            result.Disposition);
        Assert.Equal(
            ZLinkFrameworkRelocationReason.ShutdownRequested,
            result.BlockedReason);
    }

    [Fact]
    public async Task Partial_commit_restores_serving_without_force_stop()
    {
        var probe = new DrainExecutionProbe();
        var admission = new ZLinkDrainAdmissionGate();
        var operations = probe.Operations with
        {
            RestoreServing = _ =>
            {
                probe.Events.Add("restore-serving");
                return ValueTask.FromResult(true);
            },
            ReopenRelocationAdmissions = (_, _) =>
            {
                probe.Events.Add("reopen-admission");
                admission.Reset();
                return true;
            },
            DrainRelocationWorkloads = _ => ValueTask.FromResult(
                new ZLinkRelocationWorkloadDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    1,
                    true))
        };
        using var coordinator = new ZLinkDrainCoordinator(
            admission,
            new ZLinkFrameworkDrainExecutor(
                operations,
                new ZLinkLocationOptions()));

        var result = await coordinator.DrainAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1));

        var blocked = Assert.IsType<DrainBlocked>(result);
        Assert.Equal(ZLinkFrameworkRelocationReason.RelocationFailed, blocked.Reason);
        Assert.Contains("restore-serving", probe.Events);
        Assert.Contains("reopen-admission", probe.Events);
        Assert.DoesNotContain("stop-runtime", probe.Events);
        Assert.True(coordinator.IsReady);
    }

    [Fact]
    public async Task Partial_commit_without_source_terminalization_fails_closed()
    {
        var probe = new DrainExecutionProbe();
        var operations = probe.Operations with
        {
            RestoreServing = _ =>
            {
                probe.Events.Add("restore-serving");
                return ValueTask.FromResult(true);
            },
            DrainRelocationWorkloads = _ => ValueTask.FromResult(
                new ZLinkRelocationWorkloadDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    1,
                    false))
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var result = await executor.ExecuteWithProgressAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1),
            null,
            CancellationToken.None);

        Assert.Equal(
            ZLinkDrainExecutionDisposition.ForceStop,
            result.Disposition);
        Assert.Equal(ZLinkDrainForceReason.TeardownFailed, result.ForceReason);
        Assert.Equal<ulong>(1, result.CommittedUnitCount);
        Assert.DoesNotContain("restore-serving", probe.Events);
    }

    [Fact]
    public async Task Stream_session_drain_deadline_is_reported_as_deadline_exceeded()
    {
        var probe = new DrainExecutionProbe();
        using var deadline = new CancellationTokenSource();
        var operations = probe.Operations with
        {
            HasAutoConnect = false,
            HasLocationRuntime = false,
            DrainStreamSessions = _ =>
            {
                deadline.Cancel();
                return ValueTask.FromResult(false);
            }
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var result = await executor.ExecuteWithProgressAsync(
            ZLinkFrameworkLifecycleIntent.Shutdown,
            TimeSpan.FromSeconds(1),
            null,
            deadline.Token);

        Assert.Equal(
            ZLinkDrainExecutionDisposition.ForceStop,
            result.Disposition);
        Assert.Equal(
            ZLinkDrainForceReason.DeadlineExceeded,
            result.ForceReason);
    }

    [Fact]
    public async Task Stream_session_drain_failure_without_deadline_stays_teardown_failed()
    {
        var probe = new DrainExecutionProbe();
        var operations = probe.Operations with
        {
            HasAutoConnect = false,
            HasLocationRuntime = false,
            DrainStreamSessions = _ => ValueTask.FromResult(false)
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var result = await executor.ExecuteWithProgressAsync(
            ZLinkFrameworkLifecycleIntent.Shutdown,
            TimeSpan.FromSeconds(1),
            null,
            CancellationToken.None);

        Assert.Equal(
            ZLinkDrainExecutionDisposition.ForceStop,
            result.Disposition);
        Assert.Equal(
            ZLinkDrainForceReason.TeardownFailed,
            result.ForceReason);
    }

    [Fact]
    public async Task Partial_commit_shutdown_takeover_returns_shutdown_blocked()
    {
        var probe = new DrainExecutionProbe();
        ZLinkFrameworkDrainExecutor? executor = null;
        ZLinkRelocationRollbackLease? lease = null;
        var operations = probe.Operations with
        {
            AcquireRelocationRollback = _ =>
            {
                lease = new ZLinkRelocationRollbackLease();
                return lease;
            },
            RestoreServing = _ =>
            {
                probe.Events.Add("restore-serving");
                lease!.Invalidate();
                executor!.RequestShutdown(TimeSpan.FromSeconds(1));
                return ValueTask.FromResult(true);
            },
            DrainRelocationWorkloads = _ => ValueTask.FromResult(
                new ZLinkRelocationWorkloadDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.RelocationFailed,
                    1,
                    true))
        };
        executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var result = await executor.ExecuteWithProgressAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1),
            null,
            CancellationToken.None);

        Assert.Equal(
            ZLinkDrainExecutionDisposition.Blocked,
            result.Disposition);
        Assert.Equal(
            ZLinkFrameworkRelocationReason.ShutdownRequested,
            result.BlockedReason);
        Assert.DoesNotContain("reopen-admission", probe.Events);
    }

    [Fact]
    public async Task Shutdown_request_seals_and_finishes_only_the_current_relocation_unit()
    {
        var probe = new DrainExecutionProbe();
        var currentUnitEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseCurrentUnit = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var unitCount = 0;
        var operations = probe.Operations with
        {
            DrainRelocationWorkloads = async control =>
            {
                Interlocked.Increment(ref unitCount);
                currentUnitEntered.TrySetResult();
                await releaseCurrentUnit.Task
                    .WaitAsync(control.CancellationToken)
                    .ConfigureAwait(false);
                return new ZLinkRelocationWorkloadDrainResult(
                    false,
                    null,
                    1);
            }
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions
            {
                PollingInterval = TimeSpan.FromMilliseconds(1)
            });

        var relocation = executor.ExecuteAsync(
                ZLinkFrameworkLifecycleIntent.Relocate,
                TimeSpan.FromSeconds(30),
                CancellationToken.None)
            .AsTask();
        await currentUnitEntered.Task.WaitAsync(TimeSpan.FromSeconds(2));

        executor.RequestShutdown(TimeSpan.FromSeconds(2));
        Assert.Contains("seal-admission", probe.Events);
        releaseCurrentUnit.TrySetResult();

        var blocked = await Assert.ThrowsAsync<ZLinkDrainBlockedException>(
            () => relocation);
        Assert.Equal(
            ZLinkFrameworkRelocationReason.ShutdownRequested,
            blocked.Reason);
        Assert.Equal(1, unitCount);
    }

    [Fact]
    public async Task Shutdown_deadline_cancels_the_current_relocation_unit()
    {
        var probe = new DrainExecutionProbe();
        var currentUnitEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var operations = probe.Operations with
        {
            DrainRelocationWorkloads = async control =>
            {
                currentUnitEntered.TrySetResult();
                await Task.Delay(
                        Timeout.InfiniteTimeSpan,
                        control.CancellationToken)
                    .ConfigureAwait(false);
                return new ZLinkRelocationWorkloadDrainResult(
                    false,
                    null,
                    0);
            }
        };
        var executor = new ZLinkFrameworkDrainExecutor(
            operations,
            new ZLinkLocationOptions());

        var relocation = executor.ExecuteAsync(
                ZLinkFrameworkLifecycleIntent.Relocate,
                TimeSpan.FromSeconds(30),
                CancellationToken.None)
            .AsTask();
        await currentUnitEntered.Task.WaitAsync(TimeSpan.FromSeconds(2));

        executor.RequestShutdown(TimeSpan.FromMilliseconds(50));

        var blocked = await Assert.ThrowsAsync<ZLinkDrainBlockedException>(
            () => relocation.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(
            ZLinkFrameworkRelocationReason.ShutdownRequested,
            blocked.Reason);
    }

    [Fact]
    public async Task Relocate_Detaches_Workload_Without_Shutting_Down_Infrastructure()
    {
        var probe = new DrainExecutionProbe();
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions
            {
                PollingInterval = TimeSpan.FromMilliseconds(-5_099)
            });

        var reason = await executor.ExecuteAsync(
            ZLinkFrameworkLifecycleIntent.Relocate,
            TimeSpan.FromSeconds(1),
            () => probe.Events.Add("host-relocated"),
            CancellationToken.None);

        Assert.Null(reason);
        Assert.True(probe.Events.IndexOf("drain-per-actor-shells")
                    < probe.Events.IndexOf("seal-admission"));
        Assert.True(probe.Events.IndexOf("seal-admission")
                    < probe.Events.IndexOf("host-relocated"));
        Assert.DoesNotContain("marker", probe.Events);
        Assert.DoesNotContain("stop-runtime", probe.Events);
    }

    [Fact]
    public async Task Drain_Executor_Seals_Admission_Before_Weight_Quiescence()
    {
        var probe = new DrainExecutionProbe();
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions
            {
                // 50ms = -5.050s polling + 5s store bound + 100ms jitter.
                PollingInterval = TimeSpan.FromMilliseconds(-5_050)
            });

        await executor.ExecuteAsync(TimeSpan.FromSeconds(1), CancellationToken.None);
        Assert.True(
            probe.Events.IndexOf("seal-admission")
            < probe.Events.IndexOf("quiesce-serving-channels"));
    }

    [Fact]
    public async Task Drain_Executor_Seals_Admission_While_Weight_Propagation_Is_Pending()
    {
        var probe = new DrainExecutionProbe();
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions
            {
                // 50ms = -5.050s polling + 5s store bound + 100ms jitter.
                PollingInterval = TimeSpan.FromMilliseconds(-5_050)
            });

        var drain = executor.ExecuteAsync(TimeSpan.FromSeconds(1), CancellationToken.None).AsTask();
        await probe.ServingChannelsQuiesced.Task.WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Contains("seal-admission", probe.Events);
        Assert.Null(await drain);
        Assert.Contains("wait-accepted", probe.Events);
    }

    [Fact]
    public async Task Drain_Executor_Publishes_Marker_Before_Waiting_For_Accepted_Work()
    {
        var probe = new DrainExecutionProbe { HoldAcceptedOperations = true };
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions { PollingInterval = TimeSpan.FromMilliseconds(-5_099) });

        var drain = executor.ExecuteAsync(TimeSpan.FromSeconds(1), CancellationToken.None).AsTask();
        await probe.AcceptedOperationsSealed.Task.WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Contains("marker", probe.Events);
        Assert.Contains("quiesce-serving-channels", probe.Events);
        Assert.Equal("wait-accepted", probe.Events[^1]);
        probe.AcceptedOperationsReleased.TrySetResult();
        Assert.Null(await drain.WaitAsync(TimeSpan.FromSeconds(1)));
        Assert.True(probe.Events.IndexOf("seal-admission") < probe.Events.IndexOf("marker"));
        Assert.True(probe.Events.IndexOf("marker") < probe.Events.IndexOf("wait-accepted"));
    }

    [Fact]
    public async Task Serving_Weight_Store_Failure_Retries_After_Admission_Is_Sealed()
    {
        var probe = new DrainExecutionProbe { WeightAlwaysFails = true };
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations with { HasAutoConnect = false },
            new ZLinkLocationOptions { PollingInterval = TimeSpan.FromMilliseconds(1) });
        using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(30));

        var reason = await executor.ExecuteAsync(TimeSpan.FromMilliseconds(30), deadline.Token);

        Assert.Equal(ZLinkDrainForceReason.DrainingStatePublishFailed, reason);
        Assert.True(probe.WeightAttempts > 1);
        Assert.Equal("seal-admission", probe.Events[0]);
    }

    [Fact]
    public async Task Marker_Store_Failure_Retries_Until_Deadline_After_Admission_Seal()
    {
        var probe = new DrainExecutionProbe { MarkerAlwaysFails = true };
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions { PollingInterval = TimeSpan.FromMilliseconds(1) });
        using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(30));

        var reason = await executor.ExecuteAsync(
            TimeSpan.FromMilliseconds(30),
            deadline.Token);

        Assert.Equal(ZLinkDrainForceReason.DrainingStatePublishFailed, reason);
        Assert.True(probe.MarkerAttempts > 1);
        Assert.Equal("seal-admission", probe.Events[0]);
        Assert.All(probe.Events.Skip(1), static item => Assert.Equal("marker", item));
    }

    [Fact]
    public async Task Forced_Drain_Cleans_Owner_Before_Stopping_The_Location_Runtime()
    {
        var probe = new DrainExecutionProbe();
        var executor = new ZLinkFrameworkDrainExecutor(
            probe.Operations,
            new ZLinkLocationOptions(),
            stopMeshMonitoring: () => probe.Events.Add("stop-mesh-monitoring"));

        await executor.ForceStopAsync(
            ZLinkDrainForceReason.DeadlineExceeded,
            CancellationToken.None);

        Assert.Equal(
            new[]
            {
                "stop-mesh-monitoring",
                "stop-runtime",
                "stop-auto-connect",
                "cleanup-owner",
                "stop-location"
            },
            probe.Events);
    }

    [Fact]
    public async Task Fixed_Drain_Waits_For_Spot_Queue_Close_Before_Row_Release()
    {
        var closeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var allowClose = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var events = new List<string>();

        var operation = ZLinkSpotNodeCatalog.CloseBeforeReleaseAsync(
            async () =>
            {
                events.Add("close-started");
                closeStarted.TrySetResult();
                await allowClose.Task;
                events.Add("queue-drained-and-closed");
            },
            () =>
            {
                events.Add("row-released");
                return ValueTask.CompletedTask;
            }).AsTask();

        await closeStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal(new[] { "close-started" }, events);
        Assert.False(operation.IsCompleted);
        allowClose.TrySetResult();
        await operation.WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Equal(
            new[] { "close-started", "queue-drained-and-closed", "row-released" },
            events);
    }

    [Fact]
    public void Draining_Gate_Rejects_Each_New_Public_Admission_With_The_Frozen_Error()
    {
        var gate = new ZLinkDrainAdmissionGate();
        Assert.True(gate.BeginDrain());

        var spot = Assert.Throws<ZLinkFrameworkException>(gate.RequireSpotAdmission);
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, spot.Kind);

        var actor = Assert.Throws<ZLinkFrameworkException>(gate.RequireActorAdmission);
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, actor.Kind);

        Assert.False(gate.TryEnterActorAdmission(out var rejectedJoin));
        rejectedJoin.Dispose();
    }

    [Fact]
    public void Shutdown_owner_prevents_a_relocation_fence_from_reopening_admission()
    {
        var gate = new ZLinkDrainAdmissionGate();
        Assert.True(gate.TryBeginRelocationFence(
            static snapshot => snapshot.ActiveCount == 0));
        gate.Seal();
        Assert.True(gate.BeginDrain(ZLinkDrainOwner.Shutdown));

        Assert.False(gate.TryReopenRelocationFence(static () => true));
        Assert.True(gate.IsDraining);
        Assert.True(gate.IsSealed);
    }

    [Fact]
    public async Task Drain_Waits_For_Actor_Admission_Accepted_Before_The_Transition()
    {
        var gate = new ZLinkDrainAdmissionGate();
        Assert.True(gate.TryEnterActorAdmission(out var accepted));
        Assert.True(gate.BeginDrain());
        Assert.False(gate.TryEnterActorAdmission(out var rejected));
        rejected.Dispose();

        var wait = gate.WaitForAcceptedActorAdmissionsAsync(CancellationToken.None);
        Assert.False(wait.IsCompleted);

        accepted.Dispose();
        await wait.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task Drain_Does_Not_Complete_Until_The_InFlight_Executor_Completes()
    {
        var executor = new FakeDrainExecutor();
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);

        var drain = coordinator.DrainAsync(TimeSpan.FromSeconds(3)).AsTask();
        await executor.Started.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.False(drain.IsCompleted);

        executor.Complete.TrySetResult(null);

        Assert.IsType<Drained>(await drain.WaitAsync(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task Executor_Reported_Marker_Publish_Failure_Is_The_Exact_ForceStopped_Reason()
    {
        var executor = new FakeDrainExecutor();
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);

        var drain = coordinator.DrainAsync(TimeSpan.FromSeconds(3)).AsTask();
        await executor.Started.Task.WaitAsync(TimeSpan.FromSeconds(1));
        executor.Complete.TrySetResult(ZLinkDrainForceReason.DrainingStatePublishFailed);

        var forced = Assert.IsType<ForceStopped>(
            await drain.WaitAsync(TimeSpan.FromSeconds(1)));
        Assert.Equal(ZLinkDrainForceReason.DrainingStatePublishFailed, forced.Reason);
        Assert.Equal(ZLinkDrainForceReason.DrainingStatePublishFailed, executor.ForceReason);
    }

    [Fact]
    public async Task Default_Drain_Uses_Thirty_Seconds_Without_Event_Registration()
    {
        var executor = new FakeDrainExecutor();
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);

        var drain = coordinator.DrainAsync().AsTask();
        Assert.Equal(
            TimeSpan.FromSeconds(30),
            await executor.Started.Task.WaitAsync(TimeSpan.FromSeconds(1)));
        executor.Complete.TrySetResult(null);

        Assert.IsType<Drained>(await drain.WaitAsync(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task Host_Stop_Uses_The_Same_Thirty_Second_Default_Deadline()
    {
        var executor = new FakeDrainExecutor();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0);
        builder.Services.AddSingleton<IZLinkDrainExecutor>(executor);
        using var host = builder.Build();
        await host.StartAsync();

        var stop = host.StopAsync();
        var executorDeadline = await executor.Started.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.InRange(
            executorDeadline,
            TimeSpan.FromSeconds(29),
            TimeSpan.FromSeconds(30));
        Assert.False(stop.IsCompleted);
        executor.Complete.TrySetResult(null);

        await stop.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task Waiter_Cancellation_Does_Not_Cancel_Shared_Drain()
    {
        var executor = new FakeDrainExecutor();
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);
        using var waiterCancellation = new CancellationTokenSource();
        var canceledWaiter = coordinator.AwaitDrainedAsync(waiterCancellation.Token).AsTask();

        var drain = coordinator.DrainAsync(TimeSpan.FromSeconds(3)).AsTask();
        waiterCancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => canceledWaiter);

        executor.Complete.TrySetResult(null);
        var result = await drain;
        Assert.Same(result, await coordinator.AwaitDrainedAsync());
        Assert.IsType<Drained>(result);
    }

    [Fact]
    public async Task Deadline_Validation_Happens_Before_Drain_Starts()
    {
        var executor = new FakeDrainExecutor();
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(async () =>
            await coordinator.DrainAsync(TimeSpan.Zero));

        Assert.True(coordinator.IsReady);
        Assert.Equal(0, executor.ExecuteCount);
    }

    [Fact]
    public async Task Deadline_Expiry_Force_Stops_With_The_Frozen_Reason()
    {
        var executor = new FakeDrainExecutor { WaitForDeadline = true };
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);

        var result = await coordinator.DrainAsync(TimeSpan.FromMilliseconds(20));

        var forced = Assert.IsType<ForceStopped>(result);
        Assert.Equal(ZLinkDrainForceReason.DeadlineExceeded, forced.Reason);
        Assert.Equal(ZLinkDrainForceReason.DeadlineExceeded, executor.ForceReason);
    }

    [Fact]
    public async Task Deadline_Expiry_Gives_Force_Teardown_An_Independent_Bounded_Budget()
    {
        var executor = new FakeDrainExecutor
        {
            WaitForDeadline = true,
            RequireForceStopBudget = true
        };
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);

        var forced = Assert.IsType<ForceStopped>(
            await coordinator.DrainAsync(TimeSpan.FromMilliseconds(20)));

        Assert.Equal(ZLinkDrainForceReason.DeadlineExceeded, forced.Reason);
        Assert.Equal(ZLinkDrainForceReason.DeadlineExceeded, executor.ForceReason);
    }

    [Fact]
    public async Task Force_Stop_Reports_Owner_Cleanup_Failure_As_The_Terminal_Reason()
    {
        var executor = new FakeDrainExecutor
        {
            ForceFailureReason = ZLinkDrainForceReason.OwnerCleanupFailed
        };
        using var coordinator = new ZLinkDrainCoordinator(
            new ZLinkDrainAdmissionGate(),
            executor);

        var drain = coordinator.DrainAsync(TimeSpan.FromSeconds(1)).AsTask();
        await executor.Started.Task;
        executor.Complete.TrySetResult(ZLinkDrainForceReason.DeadlineExceeded);

        var forced = Assert.IsType<ForceStopped>(await drain);
        Assert.Equal(ZLinkDrainForceReason.OwnerCleanupFailed, forced.Reason);
    }

    [Fact]
    public void Actor_Drain_Uses_Only_The_Mesh_That_Owns_The_Actor_Factory()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes.Add(
            "rooms",
            new ZLinkSpotNodeRegistration
            {
                SpotNodeName = "rooms",
                SpotMeshChannelName = "room-mesh"
            });
        var actors = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "actors",
            SpotMeshChannelName = "actor-mesh"
        };
        actors.ActorFactories.Add("player", typeof(object));
        registration.SpotNodes.Add("actors", actors);

        Assert.Equal(
            "actor-mesh",
            ZLinkFrameworkRuntime.ResolveActorDrainMeshName(registration, "player"));
        Assert.Null(ZLinkFrameworkRuntime.ResolveActorDrainMeshName(registration, "unknown"));
    }

    [Fact]
    public void Actor_Drain_Retries_Only_Target_Local_Failures()
    {
        Assert.True(ZLinkActorDrainCoordinator.IsTargetLocalRetriable(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "target left",
                ZLinkRetryAdvice.RetryAfterBackoff)));
        Assert.False(ZLinkActorDrainCoordinator.IsTargetLocalRetriable(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                "durable state is invalid",
                ZLinkRetryAdvice.DoNotRetry)));
        Assert.False(ZLinkActorDrainCoordinator.IsTargetLocalRetriable(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                "authority terminal",
                ZLinkRetryAdvice.RetryAfterBackoff)));
    }

    [Fact]
    public void Spot_relocation_failure_mapping_preserves_precommit_reason_and_commit_boundary()
    {
        var cases = new (Exception Error, ZLinkFrameworkRelocationReason Expected)[]
        {
            (
                new OperationCanceledException(),
                ZLinkFrameworkRelocationReason.DeadlineExceeded),
            (
                new ZLinkRelocationDataLostException("invalid relocation payload"),
                ZLinkFrameworkRelocationReason.StateIncompatible),
            (
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    "deadline expired"),
                ZLinkFrameworkRelocationReason.DeadlineExceeded),
            (
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DataLost,
                    "durable state is unavailable"),
                ZLinkFrameworkRelocationReason.StateIncompatible),
            (
                new InvalidOperationException("callback failed"),
                ZLinkFrameworkRelocationReason.RelocationFailed)
        };

        foreach (var (error, expected) in cases)
        {
            Assert.Equal(
                expected,
                ZLinkSpotRetireScheduler.MapFailureReason(
                    error,
                    committed: false));
            Assert.Equal(
                ZLinkFrameworkRelocationReason.RelocationFailed,
                ZLinkSpotRetireScheduler.MapFailureReason(
                    error,
                    committed: true));
        }
    }

    [Fact]
    public async Task Drain_Health_Check_Projects_Readiness_Without_Starting_Drain()
    {
        var runtime = new MutableFrameworkRuntime();
        var registrations = new ServiceCollection()
            .AddLogging()
            .AddSingleton<IZLinkFrameworkRuntime>(runtime);
        await using var services = registrations
            .AddHealthChecks()
            .AddZLinkDrainHealthCheck()
            .Services
            .BuildServiceProvider();
        var health = services.GetRequiredService<HealthCheckService>();

        var serving = await health.CheckHealthAsync();
        Assert.Equal(HealthStatus.Healthy, serving.Status);
        Assert.Equal(HealthStatus.Healthy, serving.Entries["zlink-drain"].Status);

        runtime.IsReady = false;
        var draining = await health.CheckHealthAsync();
        Assert.Equal(HealthStatus.Unhealthy, draining.Status);
        Assert.Equal(HealthStatus.Unhealthy, draining.Entries["zlink-drain"].Status);
    }

    [Fact]
    public async Task Framework_Registration_Resolves_Host_Termination_Without_Locations()
    {
        var registrations = new ServiceCollection();
        registrations.AddZLinkFramework(_ => { });
        await using var services = registrations.BuildServiceProvider();

        var runtime = services.GetRequiredService<IZLinkFrameworkRuntime>();
        var result = await runtime.ShutdownAsync(TimeSpan.FromSeconds(1));

        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, result.Outcome);
        Assert.False(runtime.Status.IsReady);
    }

    [Fact]
    public void Legacy_Drain_Contracts_Are_Not_Public()
    {
        var contracts = typeof(IZLinkFrameworkRuntime).Assembly;
        Assert.Null(contracts.GetType(
            "Zlink.Framework.Contracts.Configuration.IZLinkDrainControl"));
        Assert.Null(contracts.GetType(
            "Zlink.Framework.Contracts.Configuration.ZLinkDrainResult"));
    }

    [Fact]
    public async Task Framework_Drain_Sends_ServerDrain_Before_Orderly_Stream_Close()
    {
        var port = FindFreeTcpPort();
        var sessionProbe = new DrainSessionProbe();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(sessionProbe);
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.AddStreamNode("drain-stream")
                .Bind($"tcp://127.0.0.1:{port}")
                .AddSession<DrainSession>();
        });
        using var host = builder.Build();
        await host.StartAsync();

        var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var closingObserved = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{port}"),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false }
            });
        connector.Disconnected += (closed, _) =>
        {
            disconnected.TrySetResult(closed.CloseReason);
            return ValueTask.CompletedTask;
        };
        connector.ObserveInbound((frame, _) =>
        {
            if (string.Equals(frame.Name, "session-closing", StringComparison.Ordinal))
                closingObserved.TrySetResult();
            return ValueTask.CompletedTask;
        });
        await connector.Connect.Async();
        await connector.Send(new DrainProbeMessage("connected"))
            .PacketName("drain.probe")
            .Async();
        await sessionProbe.Connected.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var result = await host.Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .ShutdownAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, result.Outcome);
        await closingObserved.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            ZlinkStreamCloseReason.ServerDrain,
            await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        await host.StopAsync();
    }

    [Fact]
    public async Task New_Stream_Session_After_Drain_Is_Rejected_With_ServerDrain()
    {
        var port = FindFreeTcpPort();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.AddStreamNode("drain-stream")
                .Bind($"tcp://127.0.0.1:{port}")
                .AddSession<DrainSession>();
        });
        using var host = builder.Build();
        await host.StartAsync();

        var result = await host.Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .ShutdownAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, result.Outcome);

        var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{port}"),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false }
            });
        connector.Disconnected += (closed, _) =>
        {
            disconnected.TrySetResult(closed.CloseReason);
            return ValueTask.CompletedTask;
        };

        await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await connector.Connect.Async());
        await host.StopAsync();
    }

    private sealed class FakeDrainExecutor : IZLinkDrainExecutor
    {
        public TaskCompletionSource<TimeSpan> Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource<ZLinkDrainForceReason?> Complete { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int ExecuteCount { get; private set; }

        public ZLinkDrainForceReason? ForceReason { get; private set; }

        public int ForceCount { get; private set; }

        public bool WaitForDeadline { get; init; }

        public ZLinkDrainForceReason? ForceFailureReason { get; init; }

        public bool RequireForceStopBudget { get; init; }

        public async ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
            TimeSpan deadline,
            CancellationToken deadlineToken)
        {
            ExecuteCount++;
            Started.TrySetResult(deadline);
            if (WaitForDeadline)
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, deadlineToken);
                return null;
            }
            return await Complete.Task.WaitAsync(deadlineToken);
        }

        public async ValueTask ForceStopAsync(
            ZLinkDrainForceReason reason,
            CancellationToken cancellationToken)
        {
            ForceCount++;
            ForceReason = reason;
            if (RequireForceStopBudget)
            {
                try
                {
                    await Task.Delay(
                            TimeSpan.FromMilliseconds(50),
                            cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    throw new ZLinkDrainForceException(
                        ZLinkDrainForceReason.OwnerCleanupFailed,
                        [new InvalidOperationException("force teardown budget expired")]);
                }
            }
            else
            {
                cancellationToken.ThrowIfCancellationRequested();
            }
            if (ForceFailureReason is { } failureReason)
                throw new ZLinkDrainForceException(
                    failureReason,
                    [new InvalidOperationException("owner cleanup failed")]);
            return;
        }
    }

    private sealed class DrainExecutionProbe
    {
        public List<string> Events { get; } = [];

        public TaskCompletionSource ServingChannelsQuiesced { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public bool MarkerAlwaysFails { get; init; }

        public bool WeightAlwaysFails { get; init; }

        public bool HoldAcceptedOperations { get; init; }

        public TaskCompletionSource AcceptedOperationsSealed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource AcceptedOperationsReleased { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int MarkerAttempts { get; private set; }

        public int WeightAttempts { get; private set; }

        public bool LastSpotDrainWasRelocation { get; private set; }

    public bool LastSpotDrainWasHostShutdown { get; private set; }

        public ZLinkDrainExecutionOperations Operations => new(
            HasAutoConnect: true,
            HasLocationRuntime: true,
            QuiesceServingChannels: _ =>
            {
                Events.Add("quiesce-serving-channels");
                WeightAttempts++;
                ServingChannelsQuiesced.TrySetResult();
                return ValueTask.FromResult(!WeightAlwaysFails);
            },
            MarkDraining: _ =>
            {
                Events.Add("marker");
                MarkerAttempts++;
                return ValueTask.FromResult(!MarkerAlwaysFails);
            },
            RestoreServing: _ => ValueTask.FromResult(true),
            SealApplicationAdmissions: () => Events.Add("seal-admission"),
            PublishDrainingToPeers: () => Events.Add("publish-draining"),
            WaitForAcceptedOperations: () =>
            {
                Events.Add("wait-accepted");
                AcceptedOperationsSealed.TrySetResult();
                return HoldAcceptedOperations
                    ? AcceptedOperationsReleased.Task
                    : Task.CompletedTask;
            },
            WaitForAcceptedActorHandoffs: _ =>
            {
                Events.Add("wait-accepted-handoffs");
                return Task.CompletedTask;
            },
            DrainActors: (_, _) =>
            {
                Events.Add("drain-actors");
                return ValueTask.FromResult(
                    new ZLinkActorDrainResult(true, null, 0));
            },
            DrainSpots: (relocate, hostShutdown, _, _) =>
            {
                LastSpotDrainWasRelocation = relocate;
                LastSpotDrainWasHostShutdown = hostShutdown;
                Events.Add("drain-spots");
                return ValueTask.FromResult(new ZLinkSpotDrainResult(true, 0));
            },
            DrainRelocationWorkloads: _ =>
            {
                LastSpotDrainWasRelocation = true;
                Events.Add("drain-per-actor-shells");
                Events.Add("drain-actors");
                Events.Add("drain-aggregate-spots");
                return ValueTask.FromResult(
                    new ZLinkRelocationWorkloadDrainResult(
                        true,
                        null,
                        0));
            },
            DrainStreamSessions: _ =>
            {
                Events.Add("drain-sessions");
                return ValueTask.FromResult(true);
            },
            FreezeOwnerWrites: _ =>
            {
                Events.Add("freeze-owner-writes");
                return ValueTask.CompletedTask;
            },
            CleanupOwner: _ =>
            {
                Events.Add("cleanup-owner");
                return ValueTask.CompletedTask;
            },
            GetRemainderCounts: static () => new ZLinkDrainRemainderCounts(0, 0, 0, 0),
            StopRuntime: _ =>
            {
                Events.Add("stop-runtime");
                return ValueTask.CompletedTask;
            },
            ForceStopRuntime: _ =>
            {
                Events.Add("stop-runtime");
                return ValueTask.CompletedTask;
            },
            StopAutoConnect: _ =>
            {
                Events.Add("stop-auto-connect");
                return ValueTask.CompletedTask;
            },
            StopLocation: _ =>
            {
                Events.Add("stop-location");
                return ValueTask.CompletedTask;
            },
            CaptureRelocationFence: static () => new ZLinkRelocationAdmissionFence(1),
            AcquireRelocationRollback: static _ => new ZLinkRelocationRollbackLease(),
            ReopenRelocationAdmissions: (_, _) =>
            {
                Events.Add("reopen-admission");
                return true;
            });
    }

    private sealed class MutableFrameworkRuntime : IZLinkFrameworkRuntime
    {
        public bool IsReady { get; set; } = true;

        public ZLinkFrameworkRuntimeStatus Status => new(
            IsReady
                ? ZLinkFrameworkRuntimeState.Serving
                : ZLinkFrameworkRuntimeState.Draining,
            IsReady,
            IsReady,
            null,
            null,
            null,
            0,
            DateTimeOffset.UtcNow);

        public IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(
            CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException(
                "The readiness check must not start observation.");

        public ValueTask<ZLinkFrameworkRelocationResult> RelocateAsync(
            ZLinkFrameworkRelocationOptions options,
            CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException(
                "The readiness check must not start relocation.");

        public ValueTask<ZLinkFrameworkTerminationResult> ShutdownAsync(
            TimeSpan? deadline = null,
            CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException(
                "The readiness check must not start termination.");
    }

    private static int FindFreeTcpPort()
    {
        var listener = new System.Net.Sockets.TcpListener(
            System.Net.IPAddress.Loopback,
            0);
        listener.Start();
        try
        {
            return ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }

    private sealed class DrainSessionProbe
    {
        public TaskCompletionSource Connected { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed record DrainProbeMessage(string Value);

    private sealed class DrainSession(
        IZLinkSessionContext context,
        DrainSessionProbe probe) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            probe.Connected.TrySetResult();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }
}
