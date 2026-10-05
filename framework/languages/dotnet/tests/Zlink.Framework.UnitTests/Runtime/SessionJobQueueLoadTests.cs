using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests;

public sealed class SessionJobQueueLoadTests
{
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
            using var connectConcurrency = new SemaphoreSlim(128);
            var connected = await Task.WhenAll(
                Enumerable
                    .Range(0, sessionCount)
                    .Select(async _ =>
                    {
                        await connectConcurrency.WaitAsync();
                        try
                        {
                            var connector = ZlinkStreamConnectorFactory.Create(
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
                            lock (connectors)
                                connectors.Add(connector);
                            await connector.Connect.Async();
                            return connector;
                        }
                        finally
                        {
                            connectConcurrency.Release();
                        }
                    })
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
        ) => Context.Client.Reply(new EchoReply(payload.Decode<EchoRequest>().Value)).Async();
    }
}
