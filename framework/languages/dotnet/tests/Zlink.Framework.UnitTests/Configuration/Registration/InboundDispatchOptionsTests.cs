using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests;

public sealed class InboundDispatchOptionsTests
{
    [Theory]
    [InlineData(ZLinkApplicationHwmProfile.Compact, 20UL)]
    [InlineData(ZLinkApplicationHwmProfile.LowLatency, 50UL)]
    [InlineData(ZLinkApplicationHwmProfile.Balanced, 100UL)]
    [InlineData(ZLinkApplicationHwmProfile.Throughput, 200UL)]
    public void Auto_Hwm_Uses_The_Exact_Profile_Ratio(
        ZLinkApplicationHwmProfile profile,
        ulong expected)
    {
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmProfile = profile,
            ProcessMemoryLimitBytes = 1_000
        };

        Assert.Equal(expected, ZLinkApplicationHwmResolver.Resolve(options));
    }

    [Fact]
    public void Explicit_Hwm_Does_Not_Require_A_Process_Memory_Limit()
    {
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmBytes = 0
        };

        Assert.Equal(0UL, ZLinkApplicationHwmResolver.Resolve(options));
    }

    [Fact]
    public void Auto_Hwm_Uses_The_Managed_Heap_As_An_Automatic_Candidate()
    {
        // Spec 06: an unconfigured host still starts and uses the smaller
        // available OS or managed-heap budget.
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmProfile = ZLinkApplicationHwmProfile.Balanced
        };

        var resolved = ZLinkApplicationHwmResolver.Resolve(options);

        Assert.True(resolved > 0);
        var managedHeapLimit = ZLinkApplicationHwmResolver.ReadManagedHeapLimit();
        Assert.NotNull(managedHeapLimit);
        Assert.True(resolved <= managedHeapLimit.Value / 10UL + 1UL);
        Assert.Equal(resolved, ZLinkApplicationHwmResolver.Resolve(options));
    }

    [Fact]
    public void Auto_Hwm_Uses_The_Smaller_Of_The_Os_And_Managed_Heap_Limits()
    {
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmProfile = ZLinkApplicationHwmProfile.Balanced
        };

        Assert.Equal(
            100UL,
            ZLinkApplicationHwmResolver.Resolve(options, 2_000UL, 1_000UL, 100UL));
    }

    [Fact]
    public void Auto_Hwm_Uses_The_Only_Available_Bounded_Limit()
    {
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmProfile = ZLinkApplicationHwmProfile.Balanced
        };

        Assert.Equal(
            200UL,
            ZLinkApplicationHwmResolver.Resolve(options, 2_000UL, null, 100UL));
        Assert.Equal(
            100UL,
            ZLinkApplicationHwmResolver.Resolve(options, null, 1_000UL, 100UL));
    }

    [Fact]
    public void Auto_Hwm_Falls_Back_To_Physical_Memory_When_No_Bounded_Limit_Exists()
    {
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmProfile = ZLinkApplicationHwmProfile.Balanced
        };

        Assert.Equal(
            100UL,
            ZLinkApplicationHwmResolver.Resolve(options, null, null, 1_000UL));
    }

    [Fact]
    public void Explicit_Process_Memory_Limit_Remains_Authoritative()
    {
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmProfile = ZLinkApplicationHwmProfile.Throughput,
            ProcessMemoryLimitBytes = 2_000
        };

        Assert.Equal(
            400UL,
            ZLinkApplicationHwmResolver.Resolve(options, 100UL, 100UL, 100UL));
    }

    [Fact]
    public void Auto_Hwm_Rejects_A_NonPositive_Calculation()
    {
        var options = new ZLinkInboundDispatchOptionsModel
        {
            ApplicationHwmProfile = ZLinkApplicationHwmProfile.Compact
        };

        Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkApplicationHwmResolver.Resolve(options, 1UL, null, null));
    }

    [Fact]
    public void Invalid_Profile_And_Zero_Memory_Limit_Are_Rejected()
    {
        var options = new ZLinkInboundDispatchOptionsModel();

        Assert.Throws<ZLinkConfigurationException>(() =>
            options.ApplicationHwmProfile = (ZLinkApplicationHwmProfile)99);
        Assert.Throws<ZLinkConfigurationException>(() =>
            options.ProcessMemoryLimitBytes = 0);
    }

    [Fact]
    public void Memory_Limited_Mode_Requires_A_Finite_Listener_Maximum()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ProcessMemoryLimitBytes = 1_000_000;
            options.AddRouteMesh("mesh").Listen()
                .ConfigureRouterSocket().MaxMessageSize = 0;
        });
        using var provider = services.BuildServiceProvider();

        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkFrameworkRegistrationValidator.ValidateInboundDispatch(
                provider.GetRequiredService<ZLinkFrameworkRegistration>()));

        Assert.Contains("MaxMessageSize", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Memory_Limited_Mode_Requires_A_Stream_Listener_Maximum()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ProcessMemoryLimitBytes = 1_000_000;
            options.AddStreamNode("stream")
                .Bind(0)
                .AddSession<TestSession>()
                .ConfigureSocket()
                .MaxMessageSize = 0;
        });
        using var provider = services.BuildServiceProvider();

        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkFrameworkRegistrationValidator.ValidateInboundDispatch(
                provider.GetRequiredService<ZLinkFrameworkRegistration>()));

        Assert.Contains("STREAM:stream", error.Message, StringComparison.Ordinal);
        Assert.Contains("MaxMessageSize", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Explicit_Unlimited_Mode_Does_Not_Require_A_Listener_Maximum()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.AddRouteMesh("mesh").Listen();
        });

        using var provider = services.BuildServiceProvider();
        ZLinkFrameworkRegistrationValidator.ValidateInboundDispatch(
            provider.GetRequiredService<ZLinkFrameworkRegistration>());
        Assert.Equal(0UL, provider.GetRequiredService<ZLinkFrameworkRegistration>()
            .InboundDispatchOptions.EffectiveApplicationHwmBytes);
    }

    [Fact]
    public void Application_Listener_Default_Is_A_Finite_Sixteen_Mebibytes()
    {
        Assert.Equal(16L * 1024L * 1024L, new ZLinkSocketConfig().MaxMessageSize);

        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ProcessMemoryLimitBytes = 100_000_000;
            options.AddRouteMesh("mesh").Listen();
        });
        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();

        ZLinkFrameworkRegistrationValidator.ValidateInboundDispatch(registration);

        Assert.Equal(10_000_000UL,
            registration.InboundDispatchOptions.EffectiveApplicationHwmBytes);
        Assert.Equal(16L * 1024L * 1024L,
            registration.SpotNodes["mesh"].Router!.SocketConfig.MaxMessageSize);
    }

    [Fact]
    public void Stream_Listener_Default_Is_Sixty_Four_Kibibytes()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddStreamNode("stream")
                .Bind(0)
                .AddSession<TestSession>();
        });
        using var provider = services.BuildServiceProvider();

        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        Assert.Equal(
            64L * 1024L,
            registration.StreamNodes["stream"].SocketConfig.MaxMessageSize);
    }

    private sealed class TestSession : IZLinkSession
    {
        public IZLinkSessionContext Context => null!;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }
}
