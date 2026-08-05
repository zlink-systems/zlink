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

        ctx.Options.AutoHwmProfile = AutoHwmProfile.Compact;
        Assert.Equal(AutoHwmProfile.Compact, ctx.Options.AutoHwmProfile);

        Assert.Equal(0UL, ctx.Options.AutoHwmMessageUnitBytes);
        const ulong planningUnit = (ulong)int.MaxValue + 1024UL;
        ctx.Options.AutoHwmMessageUnitBytes = planningUnit;
        Assert.Equal(planningUnit, ctx.Options.AutoHwmMessageUnitBytes);
        ctx.Options.AutoHwmMessageUnitBytes = 0UL;
        Assert.Equal(0UL, ctx.Options.AutoHwmMessageUnitBytes);
    }

    [Fact]
    public void auto_hwm_planning_unit_public_type_is_64_bit_unsigned()
    {
        var property = typeof(IContextOptions).GetProperty(
            nameof(IContextOptions.AutoHwmMessageUnitBytes));

        Assert.NotNull(property);
        Assert.Equal(typeof(ulong), property.PropertyType);
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
