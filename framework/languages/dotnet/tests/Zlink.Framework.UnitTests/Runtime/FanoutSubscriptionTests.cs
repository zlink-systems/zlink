using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class FanoutSubscriptionTests
{
    [Fact]
    public async Task Subscriber_WithoutTopics_ReceivesEveryApplicationTopic()
    {
        var probe = new FanoutSubscriptionProbe("inventory.changed");
        using var host = CreateHost(probe, static _ => { });

        await host.StartAsync();
        try
        {
            await WaitUntilReadyAsync(
                host.Services.GetRequiredService<IZLinkFanoutRuntime>(),
                "events");
            var client = host.Services.GetRequiredService<IZLinkFanoutClient>();

            await client.Publish(
                    "events",
                    "order.created",
                    new FanoutSubscriptionEvent("order.created"))
                .Async();
            await client.Publish(
                    "events",
                    "inventory.changed",
                    new FanoutSubscriptionEvent("inventory.changed"))
                .Async();
            await probe.TerminalReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(
                ["order.created", "inventory.changed"],
                probe.Values);
        }
        finally
        {
            await host.StopAsync();
        }
    }

    [Fact]
    public async Task Subscriber_WithOrderTopic_ReceivesPrefixAndExcludesPayment()
    {
        var probe = new FanoutSubscriptionProbe("order.created");
        using var host = CreateHost(
            probe,
            static fanout => fanout.Subscribe("order"));

        await host.StartAsync();
        try
        {
            await WaitUntilReadyAsync(
                host.Services.GetRequiredService<IZLinkFanoutRuntime>(),
                "events");
            var client = host.Services.GetRequiredService<IZLinkFanoutClient>();

            await client.Publish(
                    "events",
                    "payment",
                    new FanoutSubscriptionEvent("payment"))
                .Async();
            await client.Publish(
                    "events",
                    "order.created",
                    new FanoutSubscriptionEvent("order.created"))
                .Async();
            await probe.TerminalReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(["order.created"], probe.Values);
        }
        finally
        {
            await host.StopAsync();
        }
    }

    [Fact]
    public async Task RestrictedSubscriber_BecomesReadyWithoutApplicationEvents()
    {
        var probe = new FanoutSubscriptionProbe("unused");
        using var host = CreateHost(
            probe,
            static fanout => fanout.Subscribe("order"));

        await host.StartAsync();
        try
        {
            await WaitUntilReadyAsync(
                host.Services.GetRequiredService<IZLinkFanoutRuntime>(),
                "events");

            var status = host.Services
                .GetRequiredService<IZLinkFanoutRuntime>()
                .GetStatus("events");
            Assert.True(status.IsReady);
            Assert.Equal(1, status.ReadyPublisherCount);
            Assert.Empty(probe.Values);
        }
        finally
        {
            await host.StopAsync();
        }
    }

    [Fact]
    public void PublicTopicApis_RejectBeaconPrefixAndAllowNeighbors()
    {
        var registration = new ZLinkChannelRegistration
        {
            ChannelName = "events",
            AutoConnectType = ZLinkLocationAutoConnectType.Fanout
        };
        IZLinkFanoutChannelBuilder builder =
            new ZLinkFanoutChannelBuilder(registration);
        IZLinkFanoutClient client = new ZLinkFanoutClient(
            null!,
            new ZLinkFrameworkRegistration());
        var reserved = new[]
        {
            ZLinkFanoutLivenessProtocol.Topic,
            ZLinkFanoutLivenessProtocol.Topic + "\0"
        };
        var allowed = new[]
        {
            ZLinkFanoutLivenessProtocol.Topic[..^1],
            ZLinkFanoutLivenessProtocol.Topic[..^1] + "2"
        };

        foreach (var topic in reserved)
        {
            Assert.Throws<ArgumentException>(() => builder.Subscribe(topic));
            Assert.Throws<ArgumentException>(() => client.Publish(
                "events",
                topic,
                new FanoutSubscriptionEvent("reserved")));
        }

        foreach (var topic in allowed)
        {
            Assert.Same(builder, builder.Subscribe(topic));
            Assert.NotNull(client.Publish(
                "events",
                topic,
                new FanoutSubscriptionEvent("allowed")));
        }
    }

    [Fact]
    public async Task DuplicateTopicRegistration_HasSingleSubscriptionSemantics()
    {
        var probe = new FanoutSubscriptionProbe("order.completed");
        using var host = CreateHost(
            probe,
            static fanout => fanout.Subscribe("order").Subscribe("order"));
        var subscriber = host.Services
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .Channels["events"]
            .Subscriber!;
        Assert.Equal("order", Assert.Single(subscriber.Topics));

        await host.StartAsync();
        try
        {
            await WaitUntilReadyAsync(
                host.Services.GetRequiredService<IZLinkFanoutRuntime>(),
                "events");
            var client = host.Services.GetRequiredService<IZLinkFanoutClient>();

            await client.Publish(
                    "events",
                    "payment",
                    new FanoutSubscriptionEvent("payment"))
                .Async();
            await client.Publish(
                    "events",
                    "order.completed",
                    new FanoutSubscriptionEvent("order.completed"))
                .Async();
            await probe.TerminalReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(["order.completed"], probe.Values);
        }
        finally
        {
            await host.StopAsync();
        }
    }

    [Fact]
    public async Task ManualSubscriber_AppliesApplicationAndBeaconTopics()
    {
        var registration = new ZLinkFrameworkRegistration();
        var channel = new ZLinkChannelRegistration
        {
            ChannelName = "events",
            AutoConnectType = ZLinkLocationAutoConnectType.Fanout,
            Subscriber = new ZLinkChannelSubscriberCapabilityRegistration()
        };
        channel.Subscriber.Topics.Add("order");
        registration.Channels.Add(channel.ChannelName, channel);
        await using var services = new ServiceCollection().BuildServiceProvider();
        using var errors = new ZLinkRuntimeErrorSink();
        var state = new ZLinkFrameworkComponentState(
            new ZLinkDotNetBackendAdapterFactory().CreateRuntimeContext(),
            registration,
            services,
            errors,
            new object(),
            ZLinkApplicationJobQueueCapacityResolver.Resolve(
                ZLinkApplicationJobQueueProfile.Balanced,
                8,
                1));
        await using (state)
        await using (var bundle = await new ZLinkChannelBundleFactory(registration)
                         .CreateSubscriberBundleAsync(state, "events", channel))
        {
            var subscriber = Assert.IsAssignableFrom<ISubSocket>(bundle.Socket);
            var filters = Enumerable.Range(0, 3)
                .Select(subscriber.SubscriptionAt)
                .TakeWhile(static entry => entry is not null)
                .Select(static entry => entry!.Filter)
                .ToHashSet(StringComparer.Ordinal);

            Assert.True(filters.SetEquals(
                new HashSet<string>(StringComparer.Ordinal)
                {
                    "order",
                    ZLinkFanoutLivenessProtocol.Topic
                }));
        }
    }

    private static IHost CreateHost(
        FanoutSubscriptionProbe probe,
        Action<IZLinkFanoutChannelBuilder> configureSubscriber)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(probe);
        builder.Services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            var fanout = options.AddFanoutChannel("events")
                .EnablePublisher()
                .EnableSubscriber();
            configureSubscriber(fanout);
            fanout.AddHandler<
                FanoutSubscriptionHandler,
                FanoutSubscriptionEvent>();
        });
        return builder.Build();
    }

    private static async Task WaitUntilReadyAsync(
        IZLinkFanoutRuntime runtime,
        string channelName)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (!runtime.GetStatus(channelName).IsReady)
            await Task.Delay(TimeSpan.FromMilliseconds(20), timeout.Token);
    }

    private sealed record FanoutSubscriptionEvent(string Value);

    private sealed class FanoutSubscriptionHandler(FanoutSubscriptionProbe probe)
        : IZLinkFanoutHandler<FanoutSubscriptionEvent>
    {
        public ValueTask HandleAsync(
            FanoutSubscriptionEvent message,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            probe.Record(message.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class FanoutSubscriptionProbe(string terminalValue)
    {
        private readonly ConcurrentQueue<string> _values = new();

        public TaskCompletionSource TerminalReceived { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public IReadOnlyList<string> Values => _values.ToArray();

        public void Record(string value)
        {
            _values.Enqueue(value);
            if (string.Equals(value, terminalValue, StringComparison.Ordinal))
                TerminalReceived.TrySetResult();
        }
    }
}
