using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Backend.DotNet;
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
                nameof(IZLinkInboundDispatchOptions.ApplicationJobQueueProfile),
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
                nameof(ZLinkApplicationJobQueueStatus.ConfiguredProfile),
                nameof(ZLinkApplicationJobQueueStatus.EffectiveMaxQueuedApplicationJobs),
                nameof(ZLinkApplicationJobQueueStatus.EffectiveProcessorCount),
                nameof(ZLinkApplicationJobQueueStatus.PeakPermitsInUse),
                nameof(ZLinkApplicationJobQueueStatus.PermitsInUse),
                nameof(ZLinkApplicationJobQueueStatus.QueuedApplicationJobs),
                nameof(ZLinkApplicationJobQueueStatus.ReservedSupplyPermits)
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

    private static ZLinkApplicationJobQueue CreateQueue(ulong limit) =>
        new(new ZLinkApplicationJobQueueCapacity(
            ZLinkApplicationJobQueueProfile.Balanced,
            ConfiguredManualMax: limit,
            EffectiveProcessorCount: 8,
            EffectiveMaxQueuedApplicationJobs: limit));
}
