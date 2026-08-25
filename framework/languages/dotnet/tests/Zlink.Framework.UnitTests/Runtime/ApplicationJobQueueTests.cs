using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ApplicationJobQueueContractTests
{
    [Fact]
    public void Public_configuration_and_status_surface_matches_the_exact_contract()
    {
        Assert.Equal(
            new[]
            {
                ZLinkApplicationJobQueueProfile.Compact,
                ZLinkApplicationJobQueueProfile.LowLatency,
                ZLinkApplicationJobQueueProfile.Balanced,
                ZLinkApplicationJobQueueProfile.Throughput
            },
            Enum.GetValues<ZLinkApplicationJobQueueProfile>());

        Assert.Equal(
            new[]
            {
                nameof(IZLinkInboundDispatchOptions.ApplicationJobQueuePauseThresholdPercent),
                nameof(IZLinkInboundDispatchOptions.ApplicationJobQueueProfile),
                nameof(IZLinkInboundDispatchOptions.ApplicationJobQueueResumeThresholdPercent),
                nameof(IZLinkInboundDispatchOptions.CoreHwmBudgetBytes),
                nameof(IZLinkInboundDispatchOptions.CoreHwmMemoryLimitBytes),
                nameof(IZLinkInboundDispatchOptions.CoreHwmProfile),
                nameof(IZLinkInboundDispatchOptions.MaxQueuedApplicationJobs)
            },
            typeof(IZLinkInboundDispatchOptions)
                .GetProperties()
                .Select(static property => property.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());

        var configure = typeof(IZLinkFrameworkOptions)
            .GetMethod(nameof(IZLinkFrameworkOptions.ConfigureInboundDispatch));
        Assert.NotNull(configure);
        Assert.Equal(typeof(IZLinkInboundDispatchOptions), configure.ReturnType);
        Assert.Null(typeof(IZLinkFrameworkOptions).GetMethod("ConfigureCoreHwm"));

        Assert.Equal(
            new[]
            {
                nameof(ZLinkApplicationJobQueueStatus.CapacityWaitCount),
                nameof(ZLinkApplicationJobQueueStatus.CapacityWaitDuration),
                nameof(ZLinkApplicationJobQueueStatus.CapacityWaiters),
                nameof(ZLinkApplicationJobQueueStatus.ConfiguredManualMax),
                nameof(ZLinkApplicationJobQueueStatus.ConfiguredPauseThresholdPercent),
                nameof(ZLinkApplicationJobQueueStatus.ConfiguredProfile),
                nameof(ZLinkApplicationJobQueueStatus.ConfiguredResumeThresholdPercent),
                nameof(ZLinkApplicationJobQueueStatus.CurrentPauseDuration),
                nameof(ZLinkApplicationJobQueueStatus.EffectiveMaxQueuedApplicationJobs),
                nameof(ZLinkApplicationJobQueueStatus.EffectiveProcessorCount),
                nameof(ZLinkApplicationJobQueueStatus.PausePermitCount),
                nameof(ZLinkApplicationJobQueueStatus.PeakPermitsInUse),
                nameof(ZLinkApplicationJobQueueStatus.PermitsInUse),
                nameof(ZLinkApplicationJobQueueStatus.PressureState),
                nameof(ZLinkApplicationJobQueueStatus.QueuedApplicationJobs),
                nameof(ZLinkApplicationJobQueueStatus.ReservedSupplyPermits),
                nameof(ZLinkApplicationJobQueueStatus.ResumePermitCount)
            },
            typeof(ZLinkApplicationJobQueueStatus)
                .GetProperties()
                .Select(static property => property.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());

        Assert.Equal(
            typeof(ZLinkHostCapacityStatus),
            typeof(ZLinkFrameworkRuntimeStatus)
                .GetProperty(nameof(ZLinkFrameworkRuntimeStatus.Capacity))!
                .PropertyType);
        Assert.NotNull(typeof(IZLinkFrameworkRuntime)
            .GetMethod(nameof(IZLinkFrameworkRuntime.ResetCapacityMetrics)));
        Assert.Null(typeof(IZLinkFrameworkRuntime)
            .GetMethod("ResetCoreHwmBudgetMetrics"));
    }
}

public sealed class ApplicationJobQueueTests
{
    [Fact]
    public void Pressure_thresholds_use_exact_rounding_and_validate_hysteresis()
    {
        var defaults = new ZLinkInboundDispatchOptionsModel();
        Assert.Equal(80U, defaults.ApplicationJobQueuePauseThresholdPercent);
        Assert.Equal(60U, defaults.ApplicationJobQueueResumeThresholdPercent);
        Assert.Throws<ZLinkConfigurationException>(() =>
            defaults.ApplicationJobQueuePauseThresholdPercent = 0);
        Assert.Throws<ZLinkConfigurationException>(() =>
            defaults.ApplicationJobQueueResumeThresholdPercent = 100);

        var resolved = ZLinkApplicationJobQueueCapacityResolver.Resolve(
            ZLinkApplicationJobQueueProfile.Balanced,
            configuredManualMax: 10,
            effectiveProcessorCount: 4,
            pauseThresholdPercent: 80,
            resumeThresholdPercent: 60);

        Assert.Equal(8UL, resolved.PausePermitCount);
        Assert.Equal(6UL, resolved.ResumePermitCount);
        Assert.Equal(
            2UL,
            ZLinkApplicationJobQueueCapacityResolver.ResolvePausePermitCount(
                3,
                50));
        Assert.Equal(
            0UL,
            ZLinkApplicationJobQueueCapacityResolver.ResolveResumePermitCount(
                3,
                33));
        Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkApplicationJobQueueCapacityResolver.Resolve(
                ZLinkApplicationJobQueueProfile.Balanced,
                10,
                4,
                pauseThresholdPercent: 60,
                resumeThresholdPercent: 60));
    }

    [Theory]
    [InlineData(16, 4, 4UL)]
    [InlineData(4, 16, 4UL)]
    [InlineData(8, null, 8UL)]
    [InlineData(0, null, 1UL)]
    public void Effective_processor_count_uses_the_minimum_known_positive_constraint(
        int runtimeProcessorCount,
        int? executorMaximum,
        ulong expected)
    {
        Assert.Equal(
            expected,
            ZLinkApplicationJobQueueCapacityResolver.ResolveEffectiveProcessorCount(
                runtimeProcessorCount,
                executorMaximum));
    }

    [Theory]
    [InlineData(ZLinkApplicationJobQueueProfile.Compact, 4UL, 128UL)]
    [InlineData(ZLinkApplicationJobQueueProfile.LowLatency, 8UL, 512UL)]
    [InlineData(ZLinkApplicationJobQueueProfile.Balanced, 16UL, 2048UL)]
    [InlineData(ZLinkApplicationJobQueueProfile.Throughput, 4UL, 1024UL)]
    public void Auto_profiles_resolve_from_the_fixed_effective_processor_count(
        ZLinkApplicationJobQueueProfile profile,
        ulong processorCount,
        ulong expectedLimit)
    {
        var resolved = ZLinkApplicationJobQueueCapacityResolver.Resolve(
            profile,
            configuredManualMax: null,
            processorCount);

        Assert.Equal(processorCount, resolved.EffectiveProcessorCount);
        Assert.Equal(expectedLimit, resolved.EffectiveMaxQueuedApplicationJobs);
    }

    [Theory]
    [InlineData(0UL)]
    [InlineData(2147483648UL)]
    public void Manual_limit_outside_the_exact_range_is_rejected_before_startup(
        ulong configuredManualMax)
    {
        Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkApplicationJobQueueCapacityResolver.Resolve(
                ZLinkApplicationJobQueueProfile.Balanced,
                configuredManualMax,
                8));
    }

    [Fact]
    public void Manual_limit_overrides_auto_and_accepts_the_exact_upper_bound()
    {
        var resolved = ZLinkApplicationJobQueueCapacityResolver.Resolve(
            ZLinkApplicationJobQueueProfile.Compact,
            int.MaxValue,
            16);

        Assert.Equal((ulong)int.MaxValue, resolved.ConfiguredManualMax);
        Assert.Equal((ulong)int.MaxValue, resolved.EffectiveMaxQueuedApplicationJobs);
    }

    [Fact]
    public async Task Saturation_waits_fifo_and_never_oversubscribes()
    {
        using var queue = CreateQueue(limit: 1);
        using var first = await queue.AcquireAsync(CancellationToken.None);
        var secondTask = queue.AcquireAsync(CancellationToken.None).AsTask();
        var thirdTask = queue.AcquireAsync(CancellationToken.None).AsTask();

        Assert.False(secondTask.IsCompleted);
        Assert.False(thirdTask.IsCompleted);
        Assert.Equal(1UL, queue.GetStatus().PermitsInUse);
        Assert.Equal(2UL, queue.GetStatus().CapacityWaiters);

        first.ReleaseForHandlerStart();
        using var second = await secondTask.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.False(thirdTask.IsCompleted);
        Assert.Equal(1UL, queue.GetStatus().PermitsInUse);

        second.ReleaseForHandlerStart();
        using var third = await thirdTask.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal(1UL, queue.GetStatus().PermitsInUse);
    }

    [Fact]
    public async Task Reserved_and_queued_permits_drive_one_hysteretic_transition_each()
    {
        using var queue = CreateQueue(limit: 10);
        var applied = new List<ReceiveFlowState>();
        using var registration = queue.RegisterReceiveFlowSocket(
            new object(),
            applied.Add);
        var leases = new List<ZLinkApplicationJobQueueLease>();
        for (var index = 0; index < 8; index++)
            leases.Add(await queue.AcquireAsync(CancellationToken.None));

        Assert.Equal(
            [ReceiveFlowState.Running, ReceiveFlowState.Paused],
            applied);
        Assert.Equal(
            ZLinkApplicationJobQueuePressureState.Paused,
            queue.GetStatus().PressureState);
        leases[0].MarkQueued();
        Assert.Equal(7UL, queue.GetStatus().ReservedSupplyPermits);
        Assert.Equal(1UL, queue.GetStatus().QueuedApplicationJobs);
        Assert.Equal(2, applied.Count);

        leases[0].ReleaseForHandlerStart();
        Assert.Equal(
            ZLinkApplicationJobQueuePressureState.Paused,
            queue.GetStatus().PressureState);
        Assert.Equal(2, applied.Count);
        leases[1].ReleaseForHandlerStart();
        Assert.Equal(
            ZLinkApplicationJobQueuePressureState.Running,
            queue.GetStatus().PressureState);
        Assert.Equal(
            [
                ReceiveFlowState.Running,
                ReceiveFlowState.Paused,
                ReceiveFlowState.Running
            ],
            applied);

        foreach (var lease in leases)
            lease.Dispose();
    }

    [Fact]
    public async Task Capacity_waiter_and_reserved_to_queued_transfer_do_not_duplicate_flow_updates()
    {
        using var queue = CreateQueue(limit: 1);
        var applied = new List<ReceiveFlowState>();
        using var registration = queue.RegisterReceiveFlowSocket(
            new object(),
            applied.Add);
        using var holder = await queue.AcquireAsync(CancellationToken.None);
        holder.MarkQueued();
        var waiting = queue.AcquireAsync(CancellationToken.None).AsTask();

        Assert.False(waiting.IsCompleted);
        Assert.Equal(
            [ReceiveFlowState.Running, ReceiveFlowState.Paused],
            applied);
        holder.ReleaseForHandlerStart();
        using var admitted = await waiting.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal(
            ZLinkApplicationJobQueuePressureState.Paused,
            queue.GetStatus().PressureState);
        Assert.Equal(2, applied.Count);

        admitted.ReleaseForHandlerStart();
        Assert.Equal(ReceiveFlowState.Running, applied[^1]);
        Assert.Equal(3, applied.Count);
    }

    [Fact]
    public async Task Paused_registration_is_absolute_and_duplicate_identity_is_applied_once()
    {
        using var queue = CreateQueue(limit: 1);
        using var lease = await queue.AcquireAsync(CancellationToken.None);
        var identity = new object();
        var applied = new List<ReceiveFlowState>();
        using var first = queue.RegisterReceiveFlowSocket(identity, applied.Add);
        using var duplicate = queue.RegisterReceiveFlowSocket(
            identity,
            _ => throw new InvalidOperationException(
                "duplicate registration must reuse the existing socket entry"));

        Assert.Equal([ReceiveFlowState.Paused], applied);
        first.Dispose();
        lease.ReleaseForHandlerStart();
        Assert.Equal(
            [ReceiveFlowState.Paused, ReceiveFlowState.Running],
            applied);
        duplicate.Dispose();
        using var unregisteredLease =
            await queue.AcquireAsync(CancellationToken.None);
        Assert.Equal(2, applied.Count);
    }

    [Fact]
    public async Task Concurrent_transition_cannot_leave_a_stale_socket_state()
    {
        using var queue = CreateQueue(limit: 2);
        var applied = new List<ReceiveFlowState>();
        using var pauseEntered = new ManualResetEventSlim();
        using var releasePause = new ManualResetEventSlim();
        var blockPause = false;
        using var registration = queue.RegisterReceiveFlowSocket(
            new object(),
            state =>
            {
                lock (applied)
                    applied.Add(state);
                if (!blockPause || state != ReceiveFlowState.Paused)
                    return;
                pauseEntered.Set();
                releasePause.Wait(TimeSpan.FromSeconds(2));
            });
        using var first = await queue.AcquireAsync(CancellationToken.None);
        blockPause = true;
        var secondTask = Task.Run(async () =>
            await queue.AcquireAsync(CancellationToken.None));

        Assert.True(pauseEntered.Wait(TimeSpan.FromSeconds(1)));
        var releaseTask = Task.Run(first.ReleaseForHandlerStart);
        Assert.True(SpinWait.SpinUntil(
            () => queue.GetStatus().PressureState
                == ZLinkApplicationJobQueuePressureState.Running,
            TimeSpan.FromSeconds(1)));
        releasePause.Set();

        using var second = await secondTask.WaitAsync(TimeSpan.FromSeconds(1));
        await releaseTask.WaitAsync(TimeSpan.FromSeconds(1));
        lock (applied)
            Assert.Equal(
                [
                    ReceiveFlowState.Running,
                    ReceiveFlowState.Paused,
                    ReceiveFlowState.Running
                ],
                applied);
    }

    [Fact]
    public async Task Deregistered_socket_invalid_state_is_expected_but_other_config_failures_are_counted()
    {
        using var queue = CreateQueue(limit: 1);
        IDisposable? closingRegistration = null;
        closingRegistration = queue.RegisterReceiveFlowSocket(
            new object(),
            state =>
            {
                if (state != ReceiveFlowState.Paused)
                    return;
                closingRegistration!.Dispose();
                throw new ZlinkConfigException(
                    ZlinkConfigException.ErrorCode.InvalidState);
            });

        using var lease = await queue.AcquireAsync(CancellationToken.None);
        Assert.Equal(0UL, queue.GetPressureMetrics().FlowStateConfigFailures);

        Assert.Throws<ZlinkConfigException>(() =>
            queue.RegisterReceiveFlowSocket(
                new object(),
                _ => throw new ZlinkConfigException(
                    ZlinkConfigException.ErrorCode.InternalError)));
        Assert.Equal(1UL, queue.GetPressureMetrics().FlowStateConfigFailures);
        queue.ResetMetrics();
        Assert.Equal(0UL, queue.GetPressureMetrics().FlowStateConfigFailures);
    }

    [Fact]
    public async Task Dispose_fences_registration_and_suppresses_a_late_running_apply()
    {
        var queue = CreateQueue(limit: 1);
        var applied = new List<ReceiveFlowState>();
        using var registration = queue.RegisterReceiveFlowSocket(
            new object(),
            applied.Add);
        using var lease = await queue.AcquireAsync(CancellationToken.None);
        Assert.Equal(
            [ReceiveFlowState.Running, ReceiveFlowState.Paused],
            applied);

        queue.Dispose();
        lease.ReleaseForHandlerStart();

        Assert.Equal(
            [ReceiveFlowState.Running, ReceiveFlowState.Paused],
            applied);
        var afterDisposeApplyCount = 0;
        Assert.Throws<ObjectDisposedException>(() =>
            queue.RegisterReceiveFlowSocket(
                new object(),
                _ => afterDisposeApplyCount++));
        Assert.Equal(0, afterDisposeApplyCount);
    }

    [Fact]
    public void Reentrant_dispose_fences_an_initial_invalid_state_without_counting_a_failure()
    {
        var queue = CreateQueue(limit: 1);

        Assert.Throws<ObjectDisposedException>(() =>
            queue.RegisterReceiveFlowSocket(
                new object(),
                _ =>
                {
                    queue.Dispose();
                    throw new ZlinkConfigException(
                        ZlinkConfigException.ErrorCode.InvalidState);
                }));

        Assert.Equal(
            0UL,
            queue.GetPressureMetrics().FlowStateConfigFailures);
    }

    [Fact]
    public async Task Dispose_does_not_wait_for_an_in_flight_receive_flow_apply()
    {
        var queue = CreateQueue(limit: 1);
        using var pauseEntered = new ManualResetEventSlim();
        using var releasePause = new ManualResetEventSlim();
        var registration = queue.RegisterReceiveFlowSocket(
            new object(),
            state =>
            {
                if (state != ReceiveFlowState.Paused)
                    return;
                pauseEntered.Set();
                releasePause.Wait();
            });
        var acquire = Task.Run(async () =>
            await queue.AcquireAsync(CancellationToken.None));
        Assert.True(pauseEntered.Wait(TimeSpan.FromSeconds(1)));

        var dispose = Task.Run(queue.Dispose);
        await dispose.WaitAsync(TimeSpan.FromSeconds(1));

        var unregisterStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var unregister = Task.Run(() =>
        {
            unregisterStarted.SetResult();
            registration.Dispose();
        });
        await unregisterStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));
        await Task.Yield();
        Assert.False(unregister.IsCompleted);

        releasePause.Set();
        using var lease = await acquire.WaitAsync(TimeSpan.FromSeconds(1));
        await unregister.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task Reset_keeps_pause_state_and_current_duration_but_restarts_epoch_totals()
    {
        var time = new ManualTimeProvider();
        using var queue = CreateQueue(limit: 2, timeProvider: time);
        using var first = await queue.AcquireAsync(CancellationToken.None);
        using var second = await queue.AcquireAsync(CancellationToken.None);
        time.Advance(TimeSpan.FromSeconds(5));

        Assert.Equal(TimeSpan.FromSeconds(5), queue.GetStatus().CurrentPauseDuration);
        Assert.Equal(TimeSpan.FromSeconds(5),
            queue.GetPressureMetrics().CumulativePauseDuration);
        queue.ResetMetrics();

        var resetStatus = queue.GetStatus();
        var resetMetrics = queue.GetPressureMetrics();
        Assert.Equal(
            ZLinkApplicationJobQueuePressureState.Paused,
            resetStatus.PressureState);
        Assert.Equal(TimeSpan.FromSeconds(5), resetStatus.CurrentPauseDuration);
        Assert.Equal(0UL, resetMetrics.PausedTransitionCount);
        Assert.Equal(TimeSpan.Zero, resetMetrics.CumulativePauseDuration);

        time.Advance(TimeSpan.FromSeconds(3));
        first.ReleaseForHandlerStart();
        var resumed = queue.GetPressureMetrics();
        Assert.Equal(ZLinkApplicationJobQueuePressureState.Running, resumed.State);
        Assert.Equal(1UL, resumed.RunningTransitionCount);
        Assert.Equal(TimeSpan.Zero, resumed.CurrentPauseDuration);
        Assert.Equal(TimeSpan.FromSeconds(3), resumed.CumulativePauseDuration);
    }

    [Fact]
    public async Task Cancelling_the_oldest_waiter_removes_it_without_leaking_capacity()
    {
        using var queue = CreateQueue(limit: 1);
        using var holder = await queue.AcquireAsync(CancellationToken.None);
        using var oldestCancellation = new CancellationTokenSource();
        var oldest = queue.AcquireAsync(oldestCancellation.Token).AsTask();
        var next = queue.AcquireAsync(CancellationToken.None).AsTask();

        oldestCancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => oldest);
        holder.ReleaseForHandlerStart();

        using var admitted = await next.WaitAsync(TimeSpan.FromSeconds(1));
        var status = queue.GetStatus();
        Assert.Equal(0UL, status.CapacityWaiters);
        Assert.Equal(1UL, status.PermitsInUse);
        Assert.Equal(2UL, status.CapacityWaitCount);
    }

    [Fact]
    public async Task Permit_is_released_at_the_handler_first_instruction_not_async_completion()
    {
        using var queue = CreateQueue(limit: 1);
        using var lease = await queue.AcquireAsync(CancellationToken.None);
        lease.MarkQueued();
        var entered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var finish = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Task? handlerTask = null;
        ZLinkHandlerMethodInvoker invoker = (_, _, _, _, _, _) =>
            handlerTask = RunHandler();

        using var scope = ZLinkApplicationJobQueueInvocation.Enter(lease);
        var invocation = ZLinkHandlerInvocationEngine.InvokeAsync(
            new object(),
            invoker);

        await entered.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
        Assert.Equal(0UL, queue.GetStatus().QueuedApplicationJobs);
        Assert.False(Assert.IsAssignableFrom<Task>(handlerTask).IsCompleted);

        finish.SetResult();
        await invocation;
        await handlerTask!;
        return;

        async Task RunHandler()
        {
            entered.SetResult();
            await finish.Task;
        }
    }

    [Fact]
    public async Task Sequential_one_to_many_handlers_reacquire_one_per_handler()
    {
        using var queue = CreateQueue(limit: 1);
        using var lease = await queue.AcquireAsync(CancellationToken.None);
        using var scope = ZLinkApplicationJobQueueInvocation.Enter(lease);

        await ZLinkApplicationJobQueueInvocation
            .EnsureQueuedPermitAsync(CancellationToken.None);
        Assert.Equal(1UL, queue.GetStatus().QueuedApplicationJobs);
        ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
        Assert.Equal(0UL, queue.GetStatus().PermitsInUse);

        await ZLinkApplicationJobQueueInvocation
            .EnsureQueuedPermitAsync(CancellationToken.None);
        Assert.Equal(1UL, queue.GetStatus().QueuedApplicationJobs);
        Assert.Equal(1UL, queue.GetStatus().PermitsInUse);
        ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
        Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
    }

    [Fact]
    public async Task Detached_async_activation_outliving_its_parent_call_retains_permit_and_payload_owner()
    {
        //  Spec 33-core-hwm-application-job-flow §8: an asynchronous activation
        //  that outlives its parent call retains BOTH the job permit and the
        //  binding envelope owner, and a terminal releases each exactly once.
        using var queue = CreateQueue(limit: 1);
        var payloadOwner = new CountingDisposable();
        var activationStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var finishActivation = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Task? detachedActivation = null;

        async Task ParentCallAsync()
        {
            var lease = await queue.AcquireAsync(CancellationToken.None);
            lease.MarkQueued();
            //  The record-carried bundle the runtime attaches to inbound
            //  records: binding envelope ownership + application job admission.
            var admission = new ZLinkApplicationJobQueueRecordOwner(
                payloadOwner,
                lease);
            detachedActivation = Task.Run(async () =>
            {
                activationStarted.SetResult();
                await finishActivation.Task;
                admission.Dispose();
                //  Exactly-once: a second terminal-path dispose is a no-op.
                admission.Dispose();
            });
            //  The parent call returns here while the detached activation
            //  still owns the permit and payload envelope.
        }

        await ParentCallAsync();
        await activationStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));

        var held = queue.GetStatus();
        Assert.Equal(1UL, held.QueuedApplicationJobs);
        Assert.Equal(1UL, held.PermitsInUse);
        Assert.Equal(0, payloadOwner.DisposeCount);
        var waiter = queue.AcquireAsync(CancellationToken.None).AsTask();
        Assert.False(waiter.IsCompleted);
        Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);

        finishActivation.SetResult();
        await detachedActivation!.WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Equal(1, payloadOwner.DisposeCount);
        using var admitted = await waiter.WaitAsync(TimeSpan.FromSeconds(1));
        var released = queue.GetStatus();
        Assert.Equal(1UL, released.PermitsInUse);
        Assert.Equal(0UL, released.CapacityWaiters);
    }

    [Fact]
    public void Only_terminal_record_kinds_bypass_dispatch_admission()
    {
        Assert.False(ZLinkMeshDispatchPump.RequiresApplicationAdmission(
            MeshRecordKind.Completion));
        Assert.False(ZLinkMeshDispatchPump.RequiresApplicationAdmission(
            MeshRecordKind.SendReady));
        Assert.True(ZLinkMeshDispatchPump.RequiresApplicationAdmission(
            MeshRecordKind.NodeSend));
        Assert.True(ZLinkMeshDispatchPump.RequiresApplicationAdmission(
            MeshRecordKind.SpotControl));
    }

    [Fact]
    public async Task Reset_preserves_current_and_rebases_peak_while_clearing_wait_totals()
    {
        using var queue = CreateQueue(limit: 2);
        using var first = await queue.AcquireAsync(CancellationToken.None);
        using var second = await queue.AcquireAsync(CancellationToken.None);
        var waiting = queue.AcquireAsync(CancellationToken.None).AsTask();
        first.ReleaseForHandlerStart();
        using var admitted = await waiting.WaitAsync(TimeSpan.FromSeconds(1));
        second.ReleaseForHandlerStart();

        Assert.Equal(2UL, queue.GetStatus().PeakPermitsInUse);
        Assert.Equal(1UL, queue.GetStatus().CapacityWaitCount);
        queue.ResetMetrics();

        var reset = queue.GetStatus();
        Assert.Equal(1UL, reset.PermitsInUse);
        Assert.Equal(reset.PermitsInUse, reset.PeakPermitsInUse);
        Assert.Equal(0UL, reset.CapacityWaitCount);
        Assert.Equal(TimeSpan.Zero, reset.CapacityWaitDuration);
    }

    [Fact]
    public async Task Waiter_started_before_reset_is_not_attributed_to_the_new_epoch()
    {
        var time = new ManualTimeProvider();
        using var queue = CreateQueue(limit: 1, timeProvider: time);
        using var holder = await queue.AcquireAsync(CancellationToken.None);
        var oldEpochWaiter = queue.AcquireAsync(CancellationToken.None).AsTask();
        time.Advance(TimeSpan.FromSeconds(5));

        queue.ResetMetrics();
        time.Advance(TimeSpan.FromSeconds(3));
        holder.ReleaseForHandlerStart();
        using var admitted = await oldEpochWaiter.WaitAsync(TimeSpan.FromSeconds(1));

        var afterOldEpochCompletion = queue.GetStatus();
        Assert.Equal(0UL, afterOldEpochCompletion.CapacityWaitCount);
        Assert.Equal(TimeSpan.Zero, afterOldEpochCompletion.CapacityWaitDuration);

        var currentEpochWaiter = queue.AcquireAsync(CancellationToken.None).AsTask();
        time.Advance(TimeSpan.FromSeconds(2));
        admitted.ReleaseForHandlerStart();
        using var current = await currentEpochWaiter.WaitAsync(TimeSpan.FromSeconds(1));

        var currentEpoch = queue.GetStatus();
        Assert.Equal(1UL, currentEpoch.CapacityWaitCount);
        Assert.Equal(TimeSpan.FromSeconds(2), currentEpoch.CapacityWaitDuration);
    }

    private static ZLinkApplicationJobQueue CreateQueue(
        ulong limit,
        TimeProvider? timeProvider = null) =>
        new(new ZLinkApplicationJobQueueCapacity(
            ZLinkApplicationJobQueueProfile.Balanced,
            ConfiguredManualMax: limit,
            EffectiveProcessorCount: 8,
            EffectiveMaxQueuedApplicationJobs: limit),
            timeProvider: timeProvider);

    private sealed class CountingDisposable : IDisposable
    {
        private int _disposeCount;

        internal int DisposeCount => Volatile.Read(ref _disposeCount);

        public void Dispose() => Interlocked.Increment(ref _disposeCount);
    }
}
