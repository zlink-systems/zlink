using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests;

// #1382: one STREAM request round trip must not wait for a TCP delayed ACK (40 ms or more).
public sealed class StreamRequestRoundTripLatencyTests(Xunit.Abstractions.ITestOutputHelper output)
{
    private const int Requests = 21;

    [Fact]
    public async Task Sequential_Tcp_Requests_Do_Not_Wait_For_Delayed_Ack()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options
                .AddStreamNode("latency.stream")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<EchoSession>();
        });
        using var host = builder.Build();
        await host.StartAsync();
        try
        {
            var endpoint = Assert.IsType<string>(
                (
                    await host
                        .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                        .GetStartedStateForRoutingAsync(CancellationToken.None)
                )
                    .StreamNodes.Values.Single()
                    .BoundEndpoint
            );
            await using var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(endpoint),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                    Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                }
            );
            await connector.Connect.Async();

            await RoundTripAsync(connector);
            var elapsed = new double[Requests];
            for (var i = 0; i < Requests; i++)
            {
                var started = Stopwatch.GetTimestamp();
                await RoundTripAsync(connector);
                elapsed[i] = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
            }

            Array.Sort(elapsed);
            var median = elapsed[Requests / 2];
            output.WriteLine($"median round trip {median:F3} ms, max {elapsed[^1]:F3} ms");
            Assert.True(
                median < 20,
                $"median round trip {median:F2} ms; all: {string.Join(", ", elapsed.Select(value => value.ToString("F2")))}"
            );

            await connector.Close.Async();
        }
        finally
        {
            await host.StopAsync();
        }
    }

    private static async Task RoundTripAsync(IZlinkStreamConnector connector)
    {
        var completed = new TaskCompletionSource<ZlinkStreamResult<EchoReply>>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector
            .Request(new EchoRequest(new string('x', 1024)))
            .PacketName(nameof(EchoRequest))
            .Timeout(TimeSpan.FromSeconds(5))
            .Submit<EchoReply>(result => completed.TrySetResult(result));
        var result = await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(result.IsSuccess);
    }

    private sealed record EchoRequest(string Value);

    private sealed record EchoReply(string Value);

    private sealed class EchoSession(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken
        )
        {
            await Context.Client.Reply(new EchoReply("reply")).Async();
        }
    }
}
