using System.Reflection;
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

/// <summary>
/// Entry-spot actor relocation scenario (spec 28-relocation-flow.ko.md:589-591):
/// the actor is created via the entry spot's OnCreateActorAsync, which is
/// the JoinEntrySpot admission-free placement (spec 15-spot-actor.ko.md:489)
/// -- no OnActorJoin roundtrip, the only join shape usable cross-language
/// today. A whole-node RelocateAsync() drain then moves it to the peer node,
/// and a post-relocation probe confirms it answers on the new owner.
/// </summary>
internal sealed class RelocationActor(string actorId, IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = actorId;
    public IZLinkActorContext Context { get; } = context;
    public byte[] ApplicationState { get; set; } = [];
    public int StateVersion { get; set; }
}

internal sealed class RelocationActorFactory : IZLinkActorFactory<RelocationActor>
{
    public ValueTask<RelocationActor> CreateAsync(
        IZLinkActorContext context, CancellationToken cancellationToken = default)
    {
        return ValueTask.FromResult(new RelocationActor(context.ActorId, context));
    }
}

internal sealed class RelocationActorAdapter : IZLinkActorRelocationAdapter<RelocationActor>
{
    public ValueTask<byte[]> CaptureAsync(
        RelocationActor actor, CancellationToken cancellationToken)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write(actor.StateVersion);
        writer.Write(actor.ApplicationState.Length);
        writer.Write(actor.ApplicationState);
        return ValueTask.FromResult(stream.ToArray());
    }

    public ValueTask RestoreAsync(
        RelocationActor actor, ReadOnlyMemory<byte> payload, CancellationToken cancellationToken)
    {
        using var stream = new MemoryStream(payload.ToArray());
        using var reader = new BinaryReader(stream);
        actor.StateVersion = reader.ReadInt32();
        var length = reader.ReadInt32();
        actor.ApplicationState = reader.ReadBytes(length);
        if (actor.ApplicationState.Length != length)
            throw new InvalidOperationException("relocation application state size changed during restore");
        return ValueTask.CompletedTask;
    }
}

internal sealed record CrossLangActorCreateReq(int StateVersion, int ApplicationStateBytes);

internal sealed record CrossLangProbeReq(string Marker);

internal sealed record CrossLangProbeRes(
    string NodeRid, int StateVersion, int ApplicationStateBytes, string Marker);

internal sealed class RelocationEntrySpot(IZLinkEntrySpotContext context)
    : IZLinkEntrySpot<RelocationActor>
{
    public const string ActorType = "cross-lang-relocation-actor-type";

    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<RelocationProbeHandler>();
    }

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        RelocationActor actor, ZLinkMessage createRequest, CancellationToken cancellationToken)
    {
        if (!createRequest.IsEmpty)
        {
            var request = createRequest.Decode<CrossLangActorCreateReq>();
            actor.StateVersion = request.StateVersion;
            var state = new byte[Math.Max(0, request.ApplicationStateBytes)];
            for (var index = 0; index < state.Length; index++) state[index] = (byte)(index % 251);
            actor.ApplicationState = state;
        }
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask OnJoinedActorAsync(RelocationActor actor, CancellationToken cancellationToken)
    {
        _ = actor;
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(RelocationActor actor, CancellationToken cancellationToken)
    {
        _ = actor;
        return ValueTask.CompletedTask;
    }
}

internal sealed class RelocationProbeHandler(TestHostEventSink sink)
    : IZLinkEntrySpotActorRequestHandler<RelocationEntrySpot, RelocationActor, CrossLangProbeReq, CrossLangProbeRes>
{
    public ValueTask<CrossLangProbeRes> HandleAsync(
        RelocationEntrySpot entrySpot,
        RelocationActor actor,
        IZLinkMessageContext context,
        CrossLangProbeReq request,
        CancellationToken cancellationToken)
    {
        var nodeRid = entrySpot.Context.NodeRid.ToString();
        sink.Append($"entry-spot-probe-served|node={nodeRid}|actor={actor.ActorId}");
        return ValueTask.FromResult(new CrossLangProbeRes(
            nodeRid, actor.StateVersion, actor.ApplicationState.Length, request.Marker));
    }
}

internal sealed class EntryRelocationSourceHostedService(
    IZLinkActorManager actors,
    IZLinkFrameworkRuntime runtime,
    TestHostEventSink sink,
    string actorId,
    int payloadBytes) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var created = await actors
            .GetOrCreate(actorId, RelocationEntrySpot.ActorType)
            .Request(new CrossLangActorCreateReq(1, payloadBytes))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(cancellationToken);
        var status = created switch
        {
            ZLinkActorCreateResult.Created => "created",
            ZLinkActorCreateResult.Existing => "existing",
            ZLinkActorCreateResult.Rejected => "rejected",
            _ => "unknown"
        };
        sink.Append($"entry-spot-create|status={status}");
        var ownerBefore = created switch
        {
            ZLinkActorCreateResult.Created c => c.Actor.NodeRid.ToString(),
            ZLinkActorCreateResult.Existing e => e.Actor.NodeRid.ToString(),
            _ => "none"
        };
        sink.Append($"entry-spot-owner-before|node={ownerBefore}");

        var relocation = await runtime.RelocateAsync(
            new ZLinkFrameworkRelocationOptions
            {
                Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                Deadline = TimeSpan.FromSeconds(30)
            },
            cancellationToken);
        sink.Append($"relocate-result|outcome={relocation.Outcome}|reason={relocation.Reason}");
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
}

internal sealed class EntryRelocationTargetHostedService(
    IZLinkActorClient actorClient,
    TestHostEventSink sink,
    string actorId,
    string nodeRid) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(60);
        CrossLangProbeRes? lastReply = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                lastReply = await actorClient
                    .RequestToActor(actorId, new CrossLangProbeReq("post-relocate-probe"))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<CrossLangProbeRes>(cancellationToken);
                if (lastReply.NodeRid == nodeRid) break;
            }
            catch (ZLinkFrameworkException)
            {
                // Actor may still be mid-relocation or not yet created; keep polling.
            }
            await Task.Delay(TimeSpan.FromMilliseconds(500), cancellationToken);
        }

        if (lastReply is null || lastReply.NodeRid != nodeRid)
        {
            sink.Append($"entry-spot-probe-timeout|last={lastReply?.NodeRid ?? "none"}");
            return;
        }
        sink.Append(
            $"entry-spot-probe|nodeRid={lastReply.NodeRid}|stateVersion={lastReply.StateVersion}"
            + $"|applicationStateBytes={lastReply.ApplicationStateBytes}");
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
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

internal sealed record TestHostSpotRouteRequest(string Value);

internal sealed record TestHostSpotRouteReply(string Value);

internal sealed record TestHostSpotRouteFailRequest(string Value);

internal sealed record TestHostSpotRouteMissingRequest(string Value);

/// <summary>
/// Test-host-side view of the shared error wire. The snake_case table mirrors
/// the canonical C++ channel_reply_writer.cpp error_code_name(); the reflection
/// helpers exist because ZLinkFrameworkException's constructor and Origin
/// property are internal to the framework assemblies, and this harness must
/// observe/produce them without touching runtime sources.
/// </summary>
internal static class TestHostErrorWire
{
    public static string Name(ZLinkFrameworkErrorKind kind) => kind switch
    {
        ZLinkFrameworkErrorKind.NotFound => "not_found",
        ZLinkFrameworkErrorKind.AlreadyExists => "already_exists",
        ZLinkFrameworkErrorKind.TypeMismatch => "type_mismatch",
        ZLinkFrameworkErrorKind.NotConfigured => "not_configured",
        ZLinkFrameworkErrorKind.Rejected => "rejected",
        ZLinkFrameworkErrorKind.Unavailable => "unavailable",
        ZLinkFrameworkErrorKind.CapacityExceeded => "capacity_exceeded",
        ZLinkFrameworkErrorKind.DeadlineExceeded => "deadline_exceeded",
        ZLinkFrameworkErrorKind.ShuttingDown => "shutting_down",
        ZLinkFrameworkErrorKind.ProtocolError => "protocol_error",
        ZLinkFrameworkErrorKind.InvalidOperation => "invalid_operation",
        ZLinkFrameworkErrorKind.DataLost => "data_lost",
        _ => "internal_failure"
    };

    public static Exception CreateFrameworkException(ZLinkFrameworkErrorKind kind, string message)
    {
        var constructor = typeof(ZLinkFrameworkException)
            .GetConstructors(BindingFlags.Instance | BindingFlags.NonPublic)
            .Single();
        var arguments = constructor.GetParameters()
            .Select(parameter => parameter.Position switch
            {
                0 => (object?)kind,
                1 => message,
                _ => null
            })
            .ToArray();
        return (Exception)constructor.Invoke(arguments);
    }

    public static string Origin(ZLinkFrameworkException exception)
    {
        var property = typeof(ZLinkFrameworkException)
            .GetProperty("Origin", BindingFlags.Instance | BindingFlags.NonPublic);
        return property?.GetValue(exception)?.ToString()?.ToLowerInvariant() ?? "unspecified";
    }
}

internal sealed class TestHostSpotRouteRequestHandler(TestHostEventSink sink)
    : IZLinkRouteRequestHandler<TestHostSpotRouteRequest, TestHostSpotRouteReply>
{
    public ValueTask<TestHostSpotRouteReply> HandleAsync(
        TestHostSpotRouteRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        sink.Append($"spot-route-server|{request.Value}|{context.SourceNodeRid}");
        return ValueTask.FromResult(new TestHostSpotRouteReply($"{request.Value}|dotnet"));
    }
}

internal sealed class TestHostSpotRouteFailRequestHandler
    : IZLinkRouteRequestHandler<TestHostSpotRouteFailRequest, TestHostSpotRouteReply>
{
    public ValueTask<TestHostSpotRouteReply> HandleAsync(
        TestHostSpotRouteFailRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        // Application-origin failure with a typed kind: the wire must preserve
        // "rejected" and must NOT carry the zlink.origin=framework marker.
        throw TestHostErrorWire.CreateFrameworkException(
            ZLinkFrameworkErrorKind.Rejected,
            "application spot route failure");
    }
}

/// <summary>
/// Runs the common cross-language spot route scenario as the .NET client:
/// (a) request/reply round trip, (b) framework error (missing handler),
/// (c) application handler failure. Records observed wire kind/origin so the
/// runner can assert them per host language.
/// </summary>
internal sealed class SpotRouteClientScenarioHostedService(
    IZLinkRouteClient client,
    TestHostEventSink sink,
    string channelName,
    string peerRid,
    string value) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var target = RoutingId.From(peerRid);
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        TestHostSpotRouteReply reply;
        while (true)
        {
            try
            {
                reply = await client
                    .RequestToNode(channelName, target, new TestHostSpotRouteRequest(value))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<TestHostSpotRouteReply>(cancellationToken);
                break;
            }
            catch (ZLinkFrameworkException exception)
                when (exception.Kind is ZLinkFrameworkErrorKind.Unavailable
                          or ZLinkFrameworkErrorKind.DeadlineExceeded
                      && DateTimeOffset.UtcNow < deadline)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
            }
            catch (ZLinkFrameworkException exception)
            {
                // Terminal (a)-failure: recorded so the runner can pin
                // known cross-language divergences.
                sink.Append(
                    $"spot-route-error|kind={TestHostErrorWire.Name(exception.Kind)}|origin={TestHostErrorWire.Origin(exception)}");
                return;
            }
        }
        sink.Append($"spot-route-reply|{reply.Value}");

        await RecordFailureAsync(
            "spot-route-missing", new TestHostSpotRouteMissingRequest(value), cancellationToken);
        await RecordFailureAsync(
            "spot-route-app-error", new TestHostSpotRouteFailRequest(value), cancellationToken);
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    private async Task RecordFailureAsync<TRequest>(
        string marker,
        TRequest request,
        CancellationToken cancellationToken)
    {
        try
        {
            _ = await client
                .RequestToNode(channelName, RoutingId.From(peerRid), request)
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<TestHostSpotRouteReply>(cancellationToken);
            sink.Append($"{marker}|unexpected-success");
        }
        catch (ZLinkFrameworkException exception)
        {
            sink.Append(
                $"{marker}|kind={TestHostErrorWire.Name(exception.Kind)}|origin={TestHostErrorWire.Origin(exception)}");
        }
    }
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
