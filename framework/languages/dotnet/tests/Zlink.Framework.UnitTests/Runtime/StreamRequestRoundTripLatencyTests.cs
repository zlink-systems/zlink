using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests;

// #1382: StreamFrameWriteTests verifies the single write that avoids TCP delayed ACK
// waits. This test verifies the sequential request/reply path over a real TCP connection.
public sealed class StreamRequestRoundTripLatencyTests
{
    private const int Requests = 21;

    [Fact]
    public async Task Sequential_Tcp_Requests_Return_Expected_Replies()
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
            for (var i = 0; i < Requests; i++)
            {
                await RoundTripAsync(connector);
            }

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
        Assert.Equal("reply", Assert.IsType<EchoReply>(result.Value).Value);
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
