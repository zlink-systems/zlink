using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Configuration;

namespace Zlink.Framework.UnitTests.Configuration;

public sealed class FanoutPublisherConfigurationTests
{
    [Fact]
    public async Task PublicBuilder_Applies_Independent_Channel_Options_And_Preserves_Core_Hwm()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddFanoutChannel("default").EnablePublisher(0);
            options
                .AddFanoutChannel("first")
                .EnablePublisher(0)
                .SetNoDrop()
                .SetSendTimeout(TimeSpan.FromTicks(1));
            options
                .AddFanoutChannel("second")
                .SetSendTimeout(TimeSpan.FromMilliseconds(625))
                .EnablePublisher(0)
                .SetNoDrop(false);
        });
        await using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        foreach (
            var (name, timeout, noDrop) in new[]
            {
                ("default", 1000, false),
                ("first", 1, true),
                ("second", 625, false),
            }
        )
        {
            await using var socket = context.CreatePubSocket();
            var coreHwm = socket.Options.SendHighWaterMark;
            ZLinkChannelBundleFactory.ApplyPublisherSocketConfig(
                socket.Options,
                registration.Channels[name]
            );
            Assert.Equal(TimeSpan.FromMilliseconds(timeout), socket.Options.SendTimeout);
            Assert.Equal(noDrop, socket.Options.NoDrop);
            Assert.Equal(TimeSpan.Zero, socket.Options.Linger);
            Assert.Equal(coreHwm, socket.Options.SendHighWaterMark);
        }
    }

    [Fact]
    public void PublicBuilder_Rejects_Invalid_Timeouts_Immediately()
    {
        foreach (
            var timeout in new[]
            {
                TimeSpan.Zero,
                TimeSpan.FromTicks(-1),
                Timeout.InfiniteTimeSpan,
                TimeSpan.FromMilliseconds(int.MaxValue).Add(TimeSpan.FromTicks(1)),
                TimeSpan.MaxValue,
            }
        )
        {
            var services = new ServiceCollection();
            Assert.Throws<ZLinkConfigurationException>(() =>
                services.AddZLinkFramework(options =>
                    options.AddFanoutChannel("events").SetSendTimeout(timeout)
                )
            );
        }
        var valid = new ServiceCollection();
        valid.AddZLinkFramework(options =>
            options
                .AddFanoutChannel("events")
                .EnablePublisher(0)
                .SetSendTimeout(TimeSpan.FromMilliseconds(int.MaxValue))
        );
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void Startup_Rejects_Publisher_Options_Without_Publisher_Role(bool timeout)
    {
        var services = new ServiceCollection();
        Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                var channel = options
                    .AddFanoutChannel("events")
                    .EnableSubscriber()
                    .Connect("tcp://127.0.0.1:5000");
                if (timeout)
                    channel.SetSendTimeout(TimeSpan.FromSeconds(1));
                else
                    channel.SetNoDrop(false);
            })
        );
    }
}
