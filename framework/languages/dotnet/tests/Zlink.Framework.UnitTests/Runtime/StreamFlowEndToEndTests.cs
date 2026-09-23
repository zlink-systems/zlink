using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;

namespace Zlink.Framework.UnitTests;

public sealed class StreamFlowEndToEndTests
{
    [Fact]
    public async Task Connector_Request_Starts_Server_Flow_At_Stream_Ingress()
    {
        var port = FindFreeTcpPort();
        var builder = Host.CreateApplicationBuilder();
        var flowLogs = new FlowLoggerProvider();
        builder.Logging.AddProvider(flowLogs);
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options
                .AddStreamNode("flow.stream")
                .Bind($"tcp://127.0.0.1:{port}")
                .AddSession<FlowSession>();
        });
        using var host = builder.Build();

        try
        {
            await host.StartAsync();
            await using var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri($"tcp://127.0.0.1:{port}"),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                    Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                }
            );
            await connector.Connect.Async();
            var completed = new TaskCompletionSource<ZlinkStreamResult<FlowReply>>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );

            connector
                .Request(new FlowRequest("request"))
                .PacketName(nameof(FlowRequest))
                .Timeout(TimeSpan.FromSeconds(5))
                .Submit<FlowReply>(result => completed.TrySetResult(result));

            var callback = await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(callback.IsSuccess);
            Assert.Equal("reply", callback.Value?.Value);
            var sessionId = Assert.IsType<string>(callback.Value?.SessionId);
            Assert.False(string.IsNullOrWhiteSpace(sessionId));

            var received = Assert.Single(
                flowLogs.Messages.Where(line =>
                    line.Contains($"packet={nameof(FlowRequest)}", StringComparison.Ordinal)
                    && line.Contains("phase=received", StringComparison.Ordinal)
                )
            );
            var flowId = ReadToken(received, "flow");
            Assert.NotNull(flowId);
            Assert.True(ZlinkStreamFlowId.IsValid(flowId));
            var lines = flowLogs
                .Messages.Where(line => line.Contains($"flow={flowId}", StringComparison.Ordinal))
                .ToArray();
            Assert.Equal(2, lines.Length);
            var replied = Assert.Single(
                lines.Where(line => line.Contains("phase=replied", StringComparison.Ordinal))
            );
            Assert.Contains($"packet={nameof(FlowRequest)}", received, StringComparison.Ordinal);
            Assert.Null(ReadToken(replied, "packet"));
            Assert.Equal(flowId, ReadToken(replied, "flow"));
            Assert.Contains("origin=inbound", received, StringComparison.Ordinal);
            Assert.Contains("origin=inbound", replied, StringComparison.Ordinal);
            var correlation = ReadToken(received, "corr");
            Assert.False(string.IsNullOrWhiteSpace(correlation));
            Assert.Equal(correlation, ReadToken(replied, "corr"));
            Assert.Equal(sessionId, ReadToken(received, "session"));
            Assert.Equal(sessionId, ReadToken(replied, "session"));

            await connector.Close.Async();
            await host.StopAsync();
        }
        finally { }
    }

    [Fact]
    public void Connector_Request_Frame_Carries_Correlation_But_No_Flow()
    {
        using var sendGate = new SemaphoreSlim(1, 1);
        var headerCodec = new ZlinkStreamHeaderCodec();
        var sender = new ZlinkStreamFrameSender(
            new ZlinkStreamConnectorOptions { Endpoint = new Uri("tcp://127.0.0.1:1") },
            headerCodec,
            null,
            sendGate,
            () => null
        );
        var frame = sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Request,
            nameof(FlowRequest),
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty),
            ZlinkStreamMetadata.Empty,
            false,
            new ZlinkStreamRequestSeq(1)
        );

        var header = headerCodec.Decode(frame.HeaderBytes);
        Assert.True(header.Flags.HasFlag(ZlinkStreamHeaderFlags.HasCorrelationId));
        Assert.False(header.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
        Assert.NotNull(header.CorrelationId);
        Assert.Null(header.FlowId);
        Assert.Null(header.FlowOrigin);
    }

    private static string? ReadToken(string line, string key)
    {
        var prefix = key + "=";
        return line.Split(' ', StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault(token => token.StartsWith(prefix, StringComparison.Ordinal))
            ?[prefix.Length..];
    }

    private static int FindFreeTcpPort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private sealed record FlowRequest(string Value);

    private sealed record FlowReply(string Value, string SessionId);

    private sealed class FlowLoggerProvider : ILoggerProvider
    {
        private readonly object _gate = new();
        private readonly List<string> _messages = [];

        public IReadOnlyList<string> Messages
        {
            get
            {
                lock (_gate)
                    return _messages.ToArray();
            }
        }

        public ILogger CreateLogger(string categoryName) =>
            categoryName == ZLinkMessageFlowTracer.LoggerCategory
                ? new FlowLogger(this)
                : Microsoft.Extensions.Logging.Abstractions.NullLogger.Instance;

        public void Dispose() { }

        private sealed class FlowLogger(FlowLoggerProvider owner) : ILogger
        {
            public IDisposable? BeginScope<TState>(TState state)
                where TState : notnull => null;

            public bool IsEnabled(LogLevel logLevel) => true;

            public void Log<TState>(
                LogLevel logLevel,
                EventId eventId,
                TState state,
                Exception? exception,
                Func<TState, Exception?, string> formatter
            )
            {
                lock (owner._gate)
                    owner._messages.Add(formatter(state, exception));
            }
        }
    }

    private sealed class FlowSession(IZLinkSessionContext context) : IZLinkSession
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
            await Context
                .Client.Reply(new FlowReply("reply", Context.SessionId))
                .Async(cancellationToken);
        }
    }
}
