using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Tests.Common;

namespace Zlink.Framework.UnitTests;

public sealed class SessionJobQueueLoadTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Stream_reply_terminal_closes_wrapper(bool handlerFailure)
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Stream_reply_terminal_closes_wrapper)
            )
        )
            return;
        using var probe = new MessageFinalizerProbe();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton<SessionCount>();
        builder.Services.AddZLinkFramework(options =>
            options
                .AddStreamNode("wrapper-lifetime")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<EchoSession>()
        );
        using var host = builder.Build();
        await host.StartAsync();
        try
        {
            var state = await host
                .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                .GetStartedStateForRoutingAsync(CancellationToken.None);
            await using var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(state.StreamNodes.Values.Single().BoundEndpoint!),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                    Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                }
            );
            await connector.Connect.Async();
            var count = await probe.CountAsync(async () =>
            {
                for (var index = 0; index < 16; index++)
                {
                    var completion = new TaskCompletionSource<ZlinkStreamResult<EchoReply>>(
                        TaskCreationOptions.RunContinuationsAsynchronously
                    );
                    connector
                        .Request(new EchoRequest(handlerFailure ? -1 : index))
                        .PacketName(nameof(EchoRequest))
                        .Timeout(TimeSpan.FromSeconds(5))
                        .Submit<EchoReply>(result => completion.TrySetResult(result));
                    var result = await completion.Task.WaitAsync(TimeSpan.FromSeconds(5));
                    Assert.Equal(!handlerFailure, result.IsSuccess);
                    if (handlerFailure)
                    {
                        Assert.Equal(ZlinkStreamErrorCode.RemoteError, result.Error!.Code);
                        Assert.Contains("wrapper-lifetime-handler-failure", result.Error.Message);
                    }
                    else
                        Assert.Equal(index, result.Value!.Value);
                }
            });
            Assert.Equal(0, count);
        }
        finally
        {
            await host.StopAsync();
        }
    }

    [Fact]
    public async Task One_hundred_sessions_connect_and_reply_with_concurrency_128()
    {
        const int sessionCount = 100;
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton<SessionCount>();
        builder.Services.AddZLinkFramework(options =>
            options
                .AddStreamNode("job-queue-load")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<EchoSession>()
        );
        using var host = builder.Build();
        var connectors = new List<IZlinkStreamConnector>(sessionCount);

        try
        {
            await host.StartAsync();
            var endpoint = Assert.IsType<string>(
                (
                    await host
                        .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                        .GetStartedStateForRoutingAsync(CancellationToken.None)
                )
                    .StreamNodes.Values.Single()
                    .BoundEndpoint
            );
            var connected = await StreamConnectionHarness.ConnectAsync(
                connectors,
                sessionCount,
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(endpoint),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    ConnectTimeout = TimeSpan.FromSeconds(30),
                    RequestTimeout = TimeSpan.FromSeconds(1),
                    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                    Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                }
            );
            await host
                .Services.GetRequiredService<SessionCount>()
                .AllConnected.WaitAsync(TimeSpan.FromSeconds(30));

            var start = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var replies = connected.Select(
                (connector, index) =>
                    Task.Run(async () =>
                    {
                        await start.Task;
                        var reply = await connector
                            .Request(new EchoRequest(index))
                            .PacketName(nameof(EchoRequest))
                            .Timeout(TimeSpan.FromSeconds(1))
                            .Async<EchoReply>();
                        Assert.Equal(index, reply.Value);
                    })
            );
            var allReplies = Task.WhenAll(replies);
            start.SetResult();
            await allReplies.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            foreach (var connector in connectors)
                await connector.DisposeAsync();
            await host.StopAsync();
        }
    }

    private sealed record EchoRequest(int Value);

    private sealed record EchoReply(int Value);

    private sealed class SessionCount
    {
        private int _connected;
        private readonly TaskCompletionSource _allConnected = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        internal Task AllConnected => _allConnected.Task;

        internal void MarkConnected()
        {
            if (Interlocked.Increment(ref _connected) == 100)
                _allConnected.TrySetResult();
        }
    }

    private sealed class EchoSession(IZLinkSessionContext context, SessionCount count)
        : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            count.MarkConnected();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken
        )
        {
            var request = payload.Decode<EchoRequest>();
            if (request.Value < 0)
                throw new InvalidOperationException("wrapper-lifetime-handler-failure");
            return Context.Client.Reply(new EchoReply(request.Value)).Async();
        }
    }
}
