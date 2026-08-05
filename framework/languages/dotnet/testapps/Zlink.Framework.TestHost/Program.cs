using System.Text;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;

internal sealed class Program
{
    private static async Task Main(string[] args)
    {
        var options = TestHostOptions.Parse(args);
        var readyFilePath = options.ReadyFilePath;

        Console.SetOut(TextWriter.Synchronized(new StreamWriter(
            Console.OpenStandardOutput(),
            new UTF8Encoding(false))
        {
            AutoFlush = true
        }));

        var builder = Host.CreateApplicationBuilder(args);
        builder.Logging.ClearProviders();

        TestHostScenarioConfigurator.Configure(builder.Services, options);
        builder.Services.AddHostedService(provider =>
            new ReadySignalHostedService(
                provider.GetRequiredService<IHostApplicationLifetime>(),
                readyFilePath,
                options.StopFilePath,
                options.Mode));

        using var host = builder.Build();
        await host.RunAsync();
    }
}

internal sealed class StartupSpotCreationHostedService(
    IZLinkSpotManager spotManager,
    string meshName) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
    {
        _ = await spotManager.Create("startup-stage")
            .InMesh(meshName)
            .Async(cancellationToken);
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        return Task.CompletedTask;
    }
}

internal sealed class ChannelStartupPublishHostedService(
    IZLinkFanoutClient publisher,
    string channelName,
    string topic,
    string value) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            await publisher.Publish(channelName, topic, new TestHostPublishedEvent(value))
                .Async(stoppingToken);

            try
            {
                await Task.Delay(TimeSpan.FromMilliseconds(200), stoppingToken);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                return;
            }
        }
    }
}

internal sealed class ChannelClientStartupRequestHostedService(
    IZLinkRouteClient client,
    TestHostEventSink sink,
    string channelName,
    string value) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var reply = await client
            .RequestToChannel(channelName, new TestHostProfileRequest(value))
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<TestHostProfileReply>(cancellationToken);
        sink.Append($"channel-client|{reply.Value}");
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        return Task.CompletedTask;
    }
}

internal sealed class SpotStartupPublishHostedService(
    IZLinkSpotPublisherClient publisher,
    string channelName,
    string topic,
    string value) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            await publisher.Publish(
                    channelName,
                    topic,
                    new StartupStageEvent(value))
                .Async(stoppingToken);

            try
            {
                await Task.Delay(TimeSpan.FromMilliseconds(200), stoppingToken);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                return;
            }
        }
    }
}

internal sealed class TestHostEventSink(string? path)
{
    private readonly object _gate = new();

    public void Append(string value)
    {
        if (string.IsNullOrWhiteSpace(path)) return;

        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);

        lock (_gate)
        {
            File.AppendAllText(path, value + Environment.NewLine);
        }
    }
}

internal sealed class StartupStageSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddSubscribe<StartupStageSubscriptionHandler>(
            Context.MeshName,
            "stage.monitor");
    }
}

internal sealed class StartupStageSubscriptionHandler(TestHostEventSink sink)
    : IZLinkSpotSubscriptionHandler<StartupStageSpot, StartupStageEvent>
{
    public ValueTask HandleAsync(
        StartupStageSpot spot,
        StartupStageEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = spot;
        _ = context;
        _ = cancellationToken;
        sink.Append(message.Value);
        return ValueTask.CompletedTask;
    }
}

internal sealed record StartupStageEvent(string Value);

internal sealed record TestHostProfileRequest(string Value);

internal sealed record TestHostProfileReply(string Value);

internal sealed record TestHostProfileSend(string Value);

internal sealed record TestHostRouteRequest(string Value);

internal sealed record TestHostRouteReply(string Value);

internal sealed class TestHostRouteRequestHandler(TestHostEventSink sink)
    : IZLinkRouteRequestHandler<TestHostRouteRequest, TestHostRouteReply>
{
    public ValueTask<TestHostRouteReply> HandleAsync(
        TestHostRouteRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        sink.Append($"route-server|{request.Value}|{context.SourceNodeRid}");
        return ValueTask.FromResult(new TestHostRouteReply($"{request.Value}|dotnet"));
    }
}

internal sealed class RouteClientStartupRequestHostedService(
    IZLinkRouteClient client,
    TestHostEventSink sink,
    string channelName,
    string value) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
        TestHostRouteReply reply;
        while (true)
        {
            try
            {
                reply = await client
                    .RequestToNode(channelName, RoutingId.From("node-route"), new TestHostRouteRequest(value))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<TestHostRouteReply>(cancellationToken);
                break;
            }
            catch (ZLinkFrameworkException exception)
                when (exception.Kind == ZLinkFrameworkErrorKind.Unavailable
                      && DateTimeOffset.UtcNow < deadline)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(25), cancellationToken);
            }
        }
        sink.Append($"route-client|{reply.Value}");
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
}

internal sealed class TestHostProfileRequestHandler
    : IZLinkRequestHandler<TestHostProfileRequest, TestHostProfileReply>
{
    public ValueTask<TestHostProfileReply> HandleAsync(
        TestHostProfileRequest request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new TestHostProfileReply(request.Value));
    }
}

internal sealed class TestHostProfileSendHandler(TestHostEventSink sink)
    : IZLinkSendHandler<TestHostProfileSend>
{
    public ValueTask HandleAsync(
        TestHostProfileSend message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        sink.Append($"channel-server-send|{message.Value}");
        return ValueTask.CompletedTask;
    }
}

internal sealed record TestHostPublishedEvent(string Value);

[ZLinkHandlerGroup("testhost-channel-events")]
internal sealed class ChannelSubscriptionEventHandler(TestHostEventSink sink)
    : IZLinkFanoutHandler<TestHostPublishedEvent>
{
    public ValueTask HandleAsync(
        TestHostPublishedEvent @event,
        CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        sink.Append(@event.Value);
        return ValueTask.CompletedTask;
    }
}

internal sealed class TestHostRawStreamRecorder(TestHostEventSink sink)
{
    public void RecordConnected(IZLinkSessionContext context)
    {
        sink.Append(
            $"connected|{context.SessionId}|{context.RoutingId?.ToHex() ?? "<null>"}|{context.LocalAddr ?? "<null>"}|{context.RemoteAddr ?? "<null>"}");
    }

    public void RecordPayload(string payload)
    {
        sink.Append($"raw|{payload}");
    }

    public void RecordError(ZLinkStreamError error)
    {
        sink.Append($"error|{error.Error}");
    }

    public void RecordDisconnected(IZLinkSessionContext context)
    {
        sink.Append($"disconnected|{context.SessionId}");
    }
}

internal sealed class StreamClientStartupRequestHostedService(
    TestHostEventSink sink,
    string endpoint,
    string value) : IHostedService
{
    private IZlinkStreamConnector? _connector;

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        _connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            RequestTimeout = TimeSpan.FromSeconds(5)
        });
        _connector.Disconnected += (disconnected, _) =>
        {
            sink.Append($"stream-disconnected|{disconnected.CloseReason}");
            return ValueTask.CompletedTask;
        };
        await _connector.Connect.Async(cancellationToken);
        var pending = _connector
            .Request(new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Json,
                Encoding.UTF8.GetBytes($"\"{value}\"")))
            .PacketName("RawPing")
            .Compress()
            .Timeout(TimeSpan.FromSeconds(5))
            .Async(cancellationToken);
        var reply = await pending;
        sink.Append($"stream-client|{Encoding.UTF8.GetString(reply.Payload.Span)}");
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_connector is not null) await _connector.DisposeAsync();
    }
}

internal sealed class TestHostRawStreamSession(
    TestHostRawStreamRecorder recorder,
    IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        recorder.RecordConnected(Context);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        recorder.RecordDisconnected(Context);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        recorder.RecordError(error);
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        _ = dispatch;
        recorder.RecordPayload(payload.Decode<string>());
        await Context.Client.Reply("pong")
            .Compress()
            .Async(cancellationToken);
    }
}
