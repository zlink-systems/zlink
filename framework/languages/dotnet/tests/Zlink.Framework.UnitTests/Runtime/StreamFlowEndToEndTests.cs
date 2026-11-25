using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests;

public sealed class StreamFlowEndToEndTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Session_Creation_Failure_Closes_Only_That_Connection(bool packetFirst)
    {
        const string nodeName = "creation.stream";
        var failures = new CreationFailure(packetFirst);
        var logs = new FlowLoggerProvider();
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.AddProvider(logs);
        builder.Services.AddSingleton(failures);
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddStreamNode(nodeName).Bind("tcp://127.0.0.1:0").AddSession<CreationSession>();
        });
        using var host = builder.Build();
        await host.StartAsync();
        var endpoint = host
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .GetListenerStatus(ZLinkListenerKind.Stream, nodeName)
            .Endpoint;
        var options = new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
        };
        await using var blocker = ZlinkStreamConnectorFactory.Create(options);
        try
        {
            if (packetFirst)
            {
                await blocker.Connect.Async();
                await failures.Blocked.Task.WaitAsync(TimeSpan.FromSeconds(5));
            }
            await using var first = ZlinkStreamConnectorFactory.Create(options);
            var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var observation = first.OnDisconnected(
                (info, _) =>
                {
                    disconnected.TrySetResult(info.CloseReason);
                    return ValueTask.CompletedTask;
                }
            );
            await first.Connect.Async();
            if (packetFirst)
            {
                var rejected = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                    await first
                        .Request(new FlowRequest("rejected"))
                        .Timeout(TimeSpan.FromSeconds(5))
                        .Async<FlowReply>()
                );
                Assert.Equal(ZlinkStreamErrorCode.RemoteError, rejected.Error.Code);
                Assert.StartsWith("internal_failure:", rejected.Error.Message);
                var flow = await logs.CreationFlow.Task.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Contains("outcome=failed", flow);
                Assert.Contains($"packet={nameof(FlowRequest)}", flow);
                Assert.Contains(failures.Error.Message, flow);
            }
            Assert.Equal(
                ZlinkStreamCloseReason.TransportError,
                await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.False(first.IsConnected);
            failures.Release.TrySetResult();
            if (packetFirst)
            {
                var heldReply = await blocker
                    .Request(new FlowRequest("existing"))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<FlowReply>();
                Assert.Equal("existing", heldReply.Value);
            }

            await using var next = ZlinkStreamConnectorFactory.Create(options);
            await next.Connect.Async();
            var reply = await next.Request(new FlowRequest("next"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<FlowReply>();
            Assert.Equal("next", reply.Value);
            Assert.True(next.IsConnected);

            var error = Assert.Single(logs.Errors.Where(entry => entry.Message.Contains(nodeName)));
            Assert.Contains(nameof(CreationSession), error.Message);
            Assert.Same(failures.Error, error.Exception);
            Assert.Equal(packetFirst, logs.CreationFlow.Task.IsCompleted);
            Assert.Equal(packetFirst ? 3 : 2, failures.Attempts);
        }
        finally
        {
            failures.Release.TrySetResult();
            await host.StopAsync();
        }
    }

    [Fact]
    public async Task Connector_Request_Starts_Server_Flow_At_Stream_Ingress()
    {
        var builder = Host.CreateApplicationBuilder();
        var flowLogs = new FlowLoggerProvider();
        var handlerGate = new FlowHandlerGate();
        builder.Logging.AddProvider(flowLogs);
        builder.Services.AddSingleton(handlerGate);
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options
                .AddStreamNode("flow.stream")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<FlowSession>();
        });
        using var host = builder.Build();

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
            var completed = new TaskCompletionSource<ZlinkStreamResult<FlowReply>>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );

            connector
                .Request(new FlowRequest("request"))
                .PacketName(nameof(FlowRequest))
                .Timeout(TimeSpan.FromSeconds(5))
                .Submit<FlowReply>(result => completed.TrySetResult(result));

            Assert.Equal(
                "request",
                await handlerGate.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5))
            );
            var dispatched = Assert.Single(
                flowLogs.Messages.Where(line =>
                    line.Contains($"packet={nameof(FlowRequest)}", StringComparison.Ordinal)
                    && line.Contains("phase=dispatched", StringComparison.Ordinal)
                )
            );
            Assert.Contains("kind=request", dispatched, StringComparison.Ordinal);
            handlerGate.Release.TrySetResult();

            var callback = await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(callback.IsSuccess);
            Assert.Equal("reply", callback.Value?.Value);
            var sessionId = Assert.IsType<string>(callback.Value?.SessionId);
            Assert.False(string.IsNullOrWhiteSpace(sessionId));

            await flowLogs.Replied.Task.WaitAsync(TimeSpan.FromSeconds(5));
            global::Zlink.Framework.UnitTests.MessageFlowDeliveryTests.WaitForDelivery();
            await connector.Close.Async();
            await host.StopAsync();

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
            Assert.Equal(3, lines.Length);
            Assert.Single(
                lines.Where(line => line.Contains("phase=dispatched", StringComparison.Ordinal))
            );
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
        }
        finally
        {
            handlerGate.Release.TrySetResult();
        }
    }

    [Fact]
    public async Task Connector_Send_RecordsDispatchedBeforeHandlerCompletes()
    {
        var builder = Host.CreateApplicationBuilder();
        var flowLogs = new FlowLoggerProvider();
        var handlerGate = new FlowHandlerGate();
        builder.Logging.AddProvider(flowLogs);
        builder.Services.AddSingleton(handlerGate);
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options
                .AddStreamNode("flow.stream")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<FlowSession>();
        });
        using var host = builder.Build();

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

            await connector.Send(new FlowRequest("send")).PacketName(nameof(FlowRequest)).Async();
            Assert.Equal("send", await handlerGate.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            var dispatched = Assert.Single(
                flowLogs.Messages.Where(line =>
                    line.Contains($"packet={nameof(FlowRequest)}", StringComparison.Ordinal)
                    && line.Contains("phase=dispatched", StringComparison.Ordinal)
                )
            );
            Assert.Contains("kind=send", dispatched, StringComparison.Ordinal);
            handlerGate.Release.TrySetResult();
            await connector.Close.Async();
            await host.StopAsync();
        }
        finally
        {
            handlerGate.Release.TrySetResult();
            await host.StopAsync();
        }
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

    private sealed record FlowRequest(string Value);

    private sealed record FlowReply(string Value, string SessionId);

    private sealed class FlowLoggerProvider : ILoggerProvider
    {
        private readonly object _gate = new();
        private readonly List<string> _messages = [];
        private readonly List<(string Message, Exception? Exception)> _errors = [];
        public TaskCompletionSource Replied { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<string> CreationFlow { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public IReadOnlyList<string> Messages
        {
            get
            {
                global::Zlink.Framework.UnitTests.MessageFlowDeliveryTests.WaitForDelivery();
                lock (_gate)
                    return _messages.ToArray();
            }
        }

        public IReadOnlyList<(string Message, Exception? Exception)> Errors
        {
            get
            {
                lock (_gate)
                    return _errors.ToArray();
            }
        }

        public ILogger CreateLogger(string categoryName) =>
            new FlowLogger(this, categoryName == "zlink.framework.dispatch");

        public void Dispose() { }

        private sealed class FlowLogger(FlowLoggerProvider owner, bool captureFlow) : ILogger
        {
            public IDisposable? BeginScope<TState>(TState state)
                where TState : notnull => null;

            public bool IsEnabled(LogLevel logLevel) => captureFlow || logLevel >= LogLevel.Error;

            public void Log<TState>(
                LogLevel logLevel,
                EventId eventId,
                TState state,
                Exception? exception,
                Func<TState, Exception?, string> formatter
            )
            {
                if (!IsEnabled(logLevel))
                    return;
                var message = formatter(state, exception);
                lock (owner._gate)
                {
                    if (captureFlow)
                        owner._messages.Add(message);
                    if (logLevel == LogLevel.Error)
                        owner._errors.Add((message, exception));
                }
                if (captureFlow && message.Contains("phase=replied", StringComparison.Ordinal))
                    owner.Replied.TrySetResult();
                if (
                    captureFlow
                    && message.Contains("reason=session_creation_failed", StringComparison.Ordinal)
                )
                    owner.CreationFlow.TrySetResult(message);
            }
        }
    }

    private sealed class CreationFailure(bool packetFirst)
    {
        public int Attempts;
        public InvalidOperationException Error { get; } = new("session constructor failed");
        public TaskCompletionSource Blocked { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Create()
        {
            var attempt = Interlocked.Increment(ref Attempts);
            if (packetFirst && attempt == 1)
            {
                Blocked.TrySetResult();
                Release.Task.WaitAsync(TimeSpan.FromSeconds(5)).GetAwaiter().GetResult();
            }
            else if (attempt == (packetFirst ? 2 : 1))
            {
                throw Error;
            }
        }
    }

    private sealed class CreationSession : IZLinkSession
    {
        public CreationSession(IZLinkSessionContext context, CreationFailure failure)
        {
            failure.Create();
            Context = context;
        }

        public IZLinkSessionContext Context { get; }

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
                .Client.Reply(new FlowReply(payload.Decode<FlowRequest>().Value, Context.SessionId))
                .Async();
        }
    }

    private sealed class FlowHandlerGate
    {
        public TaskCompletionSource<string> Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class FlowSession(IZLinkSessionContext context, FlowHandlerGate gate)
        : IZLinkSession
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
            var request = payload.Decode<FlowRequest>();
            gate.Entered.TrySetResult(request.Value);
            await gate.Release.Task.WaitAsync(cancellationToken);
            if (request.Value == "request")
                await Context.Client.Reply(new FlowReply("reply", Context.SessionId)).Async();
        }
    }
}
