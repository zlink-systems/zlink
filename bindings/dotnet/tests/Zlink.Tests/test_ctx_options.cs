using System;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_ctx_options
{
    [Fact]
    public void can_set_and_get_context_options()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        ctx.Options.IoThreads = 1;
        Assert.Equal(1, ctx.Options.IoThreads);

        int maxSockets = ctx.Options.MaxSockets;
        Assert.True(maxSockets > 0);

        ctx.Options.CoreHwmProfile = AutoHwmProfile.Compact;
        Assert.Equal(AutoHwmProfile.Compact, ctx.Options.CoreHwmProfile);
    }

    [Fact]
    public void core_hwm_budget_options_and_snapshot_are_exact_ulong_values()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        const ulong memoryLimit = 16UL * 1024UL * 1024UL;
        const ulong coreBudget = 4UL * 1024UL * 1024UL;
        ctx.Options.CoreHwmMemoryLimitBytes = memoryLimit;
        ctx.Options.CoreHwmBudgetBytes = coreBudget;
        Assert.Equal(memoryLimit, ctx.Options.CoreHwmMemoryLimitBytes);
        Assert.Equal(coreBudget, ctx.Options.CoreHwmBudgetBytes);

        ctx.RecalculateAutoHwm();
        CoreHwmBudgetSnapshot before = ctx.GetCoreHwmBudgetSnapshot();
        Assert.Equal(1U, before.AbiVersion);
        Assert.True(before.StructSize > 0U);
        Assert.Equal(memoryLimit, before.ConfiguredMemoryLimitBytes);
        Assert.Equal(coreBudget, before.ConfiguredCoreBudgetBytes);
        Assert.Equal(coreBudget, before.EffectiveCoreBudgetBytes);
        Assert.True(before.BudgetPlanningActive);
        Assert.True(before.AggregateHwmValid);
        Assert.False(before.BudgetInsufficient);
        Assert.False(before.AggregateOverflow);
        Assert.All(before.ReservedUInt64, value => Assert.Equal(0UL, value));

        ctx.ResetCoreHwmBudgetMetrics();
        CoreHwmBudgetSnapshot after = ctx.GetCoreHwmBudgetSnapshot();
        Assert.Equal(before.MeasurementEpoch + 1UL, after.MeasurementEpoch);
        Assert.Equal(before.BudgetGeneration, after.BudgetGeneration);
        Assert.Equal(before.CurrentAccountedBytes, after.CurrentAccountedBytes);
        Assert.Equal(before.ActiveDirectionalQueueCount,
            after.ActiveDirectionalQueueCount);

        ctx.Options.CoreHwmMemoryLimitBytes = 0UL;
        ctx.Options.CoreHwmBudgetBytes = 0UL;
        Assert.Equal(0UL, ctx.Options.CoreHwmMemoryLimitBytes);
        Assert.Equal(0UL, ctx.Options.CoreHwmBudgetBytes);
    }

    [Fact]
    public void context_default_limits_are_positive()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        Assert.True(ctx.Options.MaxSockets > 0);
        Assert.True(ctx.Options.SocketLimit > 0);
        Assert.True(ctx.Options.IoThreads > 0);
        Assert.True(ctx.Options.MessageThreadSize > 0);
    }

    [Fact]
    public void context_blocky_can_be_configured()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        Assert.True(ctx.Options.Blocky);
        ctx.Options.Blocky = false;
        Assert.False(ctx.Options.Blocky);
    }

    [Fact]
    public void context_thread_options_accept_valid_values()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        ctx.Options.ThreadSchedulingPolicy = 0;
        Assert.Equal(0, ctx.Options.ThreadSchedulingPolicy);

        ctx.Options.AddThreadAffinityCpu(0);
        ctx.Options.RemoveThreadAffinityCpu(0);
    }

    [Fact]
    public void context_blocky_changes_default_socket_linger()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using (var preRouter = ctx.CreateRouterSocket())
        {
            Assert.Null(preRouter.Options.Linger);
        }

        ctx.Options.Blocky = false;
        Assert.False(ctx.Options.Blocky);

        using var router = ctx.CreateRouterSocket();
        Assert.Equal(TimeSpan.Zero, router.Options.Linger);
    }

    [Fact]
    public void socket_handshake_interval_is_public_and_reaches_native_option()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        router.Options.HandshakeInterval = TimeSpan.FromMilliseconds(750);

        Assert.Equal(TimeSpan.FromMilliseconds(750), router.Options.HandshakeInterval);
    }

    [Fact]
    public void shutdown_context_is_callable()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        ctx.Shutdown();
    }
}
