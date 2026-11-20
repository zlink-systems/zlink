using System.Buffers.Binary;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.BoundSessionDisconnect;

public sealed class BoundSessionDisconnectTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Earlier_reply_or_push_precedes_close(bool push)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(o =>
        {
            o.DisableImplicitHandlerAutoRegistration();
            o.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            o.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
            var objects = o.AddRouteMesh("disconnect")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<DEntry>();
            objects.AddActorFactory<DActor, DFactory>("actor", f => f.DisableRelocation());
            o.AddStreamNode("clients")
                .Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<DSession>();
        });
        using var app = builder.Build();
        await app.StartAsync();
        var traceDirectory = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY");
        using var flow = traceDirectory is null
            ? null
            : new TestHostMessageFlowListener(Path.Combine(traceDirectory, $"ordered-{push}.flow"));
        var failures = new List<string>();
        try
        {
            var endpoint = app
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .GetListenerStatus(ZLinkListenerKind.Stream, "clients")
                .Endpoint;
            var actors = app.Services.GetRequiredService<IZLinkActorClient>();
            for (var iteration = 0; iteration < 40; iteration++)
            {
                var actor = "ordered:" + Guid.NewGuid();
                using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                using var peer = new TcpClient();
                var address = new Uri(endpoint);
                await peer.ConnectAsync(address.Host, address.Port, deadline.Token);
                var stream = peer.GetStream();
                await SendRawRequestAsync(stream, new DLogin(actor), 1, deadline.Token);
                while (true)
                {
                    var login = await ReadRawFrameAsync(stream, deadline.Token);
                    Assert.NotNull(login);
                    if (login.Value.Header.Kind == ZlinkStreamMessageKind.Control)
                        continue;
                    Assert.True(
                        login.Value.Header.Kind == ZlinkStreamMessageKind.Response,
                        $"Unexpected login reply {login.Value.Header.Kind}: {System.Text.Encoding.UTF8.GetString(login.Value.Payload)}"
                    );
                    Assert.Equal(new ZlinkStreamRequestSeq(1), login.Value.Header.RequestSeq);
                    break;
                }
                // Stop the continuation after the Actor turn and finish the next handler's
                // close before resuming. Raw reads measure frame/EOF order independently
                // of connector callback scheduling.
                DSession.BeforeReplyCompletion = push
                    ? null
                    : () =>
                        actors
                            .RequestToActor(actor, new DDrop())
                            .Async<Pong>()
                            .AsTask()
                            .GetAwaiter()
                            .GetResult();
                await SendRawRequestAsync(stream, new DOrdered(push), 2, deadline.Token);
                var events = new List<string>();
                while (await ReadRawFrameAsync(stream, deadline.Token) is { } received)
                {
                    if (received.Header.Kind == ZlinkStreamMessageKind.Control)
                        continue;
                    if (
                        received.Header.Kind == ZlinkStreamMessageKind.Send
                        && received.Header.Name == nameof(Pong)
                    )
                    {
                        Assert.NotNull(
                            ZLinkFrameworkJsonPayloadCodec.Deserialize<Pong>(received.Payload)
                        );
                        events.Add("push");
                    }
                    else if (
                        received.Header.Kind == ZlinkStreamMessageKind.Response
                        && received.Header.RequestSeq == new ZlinkStreamRequestSeq(2)
                    )
                    {
                        Assert.NotNull(
                            ZLinkFrameworkJsonPayloadCodec.Deserialize<Pong>(received.Payload)
                        );
                        events.Add("reply");
                    }
                    else
                        events.Add(received.Header.Kind.ToString());
                }
                events.Add("eof");
                var expected = push ? "push" : "reply";
                if (events.Count < 2 || events[0] != expected || events[^1] != "eof")
                    failures.Add(
                        $"iteration={iteration} expected={expected},eof actual={string.Join(',', events)}"
                    );
                DSession.BeforeReplyCompletion = null;
            }
        }
        finally
        {
            DSession.BeforeReplyCompletion = null;
            await app.StopAsync();
        }
        Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
    }

    private static async Task SendRawRequestAsync<T>(
        NetworkStream stream,
        T message,
        ulong sequence,
        CancellationToken ct
    )
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(sequence),
            typeof(T).Name,
            ZlinkStreamMetadata.Empty
        );
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            ZLinkFrameworkJsonPayloadCodec.Serialize(message)
        );
        await stream.WriteAsync(frame, ct);
    }

    private static async Task<(ZlinkStreamHeader Header, byte[] Payload)?> ReadRawFrameAsync(
        NetworkStream stream,
        CancellationToken ct
    )
    {
        var prefix = new byte[ZLinkStreamFrameCodec.PrefixSize];
        if (await stream.ReadAsync(prefix.AsMemory(0, 1), ct) == 0)
            return null;
        await stream.ReadExactlyAsync(prefix.AsMemory(1), ct);
        var length = checked(
            prefix.Length
            + BinaryPrimitives.ReadUInt16BigEndian(prefix)
            + (int)BinaryPrimitives.ReadUInt32BigEndian(prefix.AsSpan(2))
        );
        Assert.InRange(length, prefix.Length, 65536);
        var frame = new byte[length];
        prefix.CopyTo(frame, 0);
        await stream.ReadExactlyAsync(frame.AsMemory(prefix.Length), ct);
        return DecodeRawFrame(frame);
    }

    private static (ZlinkStreamHeader Header, byte[] Payload) DecodeRawFrame(byte[] frame)
    {
        Assert.True(ZLinkStreamFrameCodec.TryDecode(frame, out var header, out var payload));
        return (ZLinkStreamProtocolDefaults.DecodeHeader(header.ToArray()), payload.ToArray());
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Actor_handler_disconnect_closes_physical_connection(bool remote, bool join)
    {
        var locations = new ZLinkInMemoryProviderLocationStore();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(o =>
        {
            o.DisableImplicitHandlerAutoRegistration();
            o.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            o.AddLocationStore(locations);
            var objects = o.AddRouteMesh("disconnect")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<DEntry>();
            objects.AddActorFactory<DActor, DFactory>("actor", f => f.DisableRelocation());
            objects.AddSpotFactory<DRoom>(
                "room",
                f => f.ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide).DisableRelocation()
            );
            if (!remote)
                o.AddStreamNode("clients")
                    .Bind("ws://127.0.0.1:0")
                    .EnableActorDispatch()
                    .AddSession<DSession>();
        });
        using var app = builder.Build();
        await app.StartAsync();
        IHost? gateway = null;
        try
        {
            var room = "disconnect:" + Guid.NewGuid();
            var actor = "actor:" + Guid.NewGuid();
            await app
                .Services.GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate(room, "room")
                .InMesh("disconnect")
                .Async();
            await app
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actor, "actor")
                .InMesh("disconnect")
                .Async();
            if (remote)
            {
                var peer = app
                    .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                    .GetListenerStatus(ZLinkListenerKind.RouteMesh, "disconnect")
                    .Endpoint;
                var gatewayBuilder = Host.CreateApplicationBuilder();
                gatewayBuilder.Services.AddZLinkFramework(o =>
                {
                    o.DisableImplicitHandlerAutoRegistration();
                    o.AddLocationStore(locations);
                    var mesh = o.AddRouteMesh("disconnect").Listen("tcp://127.0.0.1:0");
                    mesh.Objects().Client();
                    mesh.PeerConnections.Connect(peer);
                    o.AddStreamNode("clients")
                        .Bind("ws://127.0.0.1:0")
                        .EnableActorDispatch()
                        .AddSession<DSession>();
                });
                gateway = gatewayBuilder.Build();
                await gateway.StartAsync();
            }
            var endpoint = (gateway ?? app)
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .GetListenerStatus(ZLinkListenerKind.Stream, "clients")
                .Endpoint;
            await using var client = ZlinkStreamConnectorFactory.Create(
                new() { Endpoint = new(endpoint), DispatchMode = ZlinkStreamDispatchMode.Immediate }
            );
            var disconnected = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var registration = client.OnDisconnected(
                (_, _) =>
                {
                    disconnected.TrySetResult();
                    return ValueTask.CompletedTask;
                }
            );
            await client.Connect.Async();
            await client.Request(new DLogin(actor)).Async<Pong>();
            // Session-Actor binding §9: after the server closes the connection the client may
            // reconnect as a new physical session without a binding, which replaces
            // DSession.Current. The cleanup below is observed on the session that bound.
            var boundSession = DSession.Current;
            if (join)
                await client.Request(new DMove(room)).Async<Pong>();
            var actors = app.Services.GetRequiredService<IZLinkActorClient>();
            var location = await actors.RequestToActor(actor, new DWhere()).Async<DLocation>();
            if (join && location.Spot != room)
                throw new InvalidOperationException("Actor did not reach User Spot.");
            await actors.RequestToActor(actor, new DDrop()).Async<Pong>();
            await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(client.IsConnected);
            await DFactory.CurrentActor.Disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, DFactory.CurrentActor.DisconnectCount);
            using var cleanupDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            while (boundSession.Actors.Bound.Count != 0)
                await Task.Delay(10, cleanupDeadline.Token);
            var absent = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await DFactory.Current.BoundSession.DisconnectAsync()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, absent.Kind);
        }
        finally
        {
            if (gateway != null)
            {
                await gateway.StopAsync();
                gateway.Dispose();
            }
            await app.StopAsync();
        }
    }
}

public sealed record Pong;

public sealed record DLogin(string Actor);

public sealed record DMove(string Room);

public sealed record DWhere;

public sealed record DDrop;

public sealed record DOrdered(bool Push);

public sealed record DLocation(string? Spot);

public sealed class DActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
    public int DisconnectCount;
    public TaskCompletionSource Disconnected { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public ValueTask ObserveDisconnected()
    {
        Interlocked.Increment(ref DisconnectCount);
        Disconnected.TrySetResult();
        return ValueTask.CompletedTask;
    }
}

public sealed class DFactory : IZLinkActorFactory<DActor>
{
    public static IZLinkActorContext Current { get; private set; } = null!;
    public static DActor CurrentActor { get; private set; } = null!;

    public ValueTask<DActor> CreateAsync(IZLinkActorContext context, CancellationToken ct = default)
    {
        Current = context;
        CurrentActor = new DActor(context);
        return ValueTask.FromResult(CurrentActor);
    }

    async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(
        IZLinkActorContext context,
        CancellationToken ct
    ) => await CreateAsync(context, ct);
}

public sealed class DSession(IZLinkSessionContext context, IZLinkActorManager actors)
    : IZLinkSession
{
    public static Action? BeforeReplyCompletion { get; set; }
    public static IZLinkSessionContext Current { get; private set; } = null!;
    public IZLinkSessionContext Context { get; } = Remember(context);

    private static IZLinkSessionContext Remember(IZLinkSessionContext context)
    {
        Current = context;
        return context;
    }

    public ValueTask OnConnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage message,
        CancellationToken ct
    )
    {
        if (dispatch.PacketName == nameof(DLogin))
        {
            var result = await actors
                .GetOrCreate(message.Decode<DLogin>().Actor, "actor")
                .InMesh("disconnect")
                .Async(ct);
            var actor = result switch
            {
                ZLinkActorCreateResult.Created created => created.Actor,
                ZLinkActorCreateResult.Existing existing => existing.Actor,
                _ => throw new InvalidOperationException("Actor creation failed"),
            };
            await Context.Actors.BindAsync(actor, ct);
            await Context.Client.Reply(new Pong()).Async();
        }
        else
        {
            var hook = BeforeReplyCompletion;
            if (dispatch.PacketName == nameof(DOrdered))
                ZLinkStateLane.WorkObserver.Value = (stage, _) =>
                {
                    if (stage == "session-actor-reply-completed")
                        hook?.Invoke();
                };
            try
            {
                await Context.Actors.Bound.Single().RelayAsync(message);
            }
            finally
            {
                ZLinkStateLane.WorkObserver.Value = null;
            }
        }
    }
}

public sealed class DEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<DActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<DMoveHandler, DActor>();
        Context.Handlers.AddActorPacket<DOrderedHandler, DActor>();
        Context.Handlers.AddActorPacket<DEntryWhereHandler, DActor>();
        Context.Handlers.AddActorPacket<DEntryDropHandler, DActor>();
    }

    public ValueTask OnJoinedActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(DActor actor, CancellationToken ct) =>
        actor.ObserveDisconnected();

    public ValueTask OnLeaveActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

public sealed class DMoveHandler : IZLinkEntrySpotActorRequestHandler<DEntry, DActor, DMove, Pong>
{
    public ValueTask<Pong> HandleAsync(
        DEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        DMove request,
        CancellationToken ct
    )
    {
        actor.Context.JoinSpot(request.Room).Defer();
        return ValueTask.FromResult(new Pong());
    }
}

public sealed class DEntryWhereHandler
    : IZLinkEntrySpotActorRequestHandler<DEntry, DActor, DWhere, DLocation>
{
    public ValueTask<DLocation> HandleAsync(
        DEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        DWhere request,
        CancellationToken ct
    ) => ValueTask.FromResult(new DLocation(actor.Context.SpotId));
}

public sealed class DEntryDropHandler
    : IZLinkEntrySpotActorRequestHandler<DEntry, DActor, DDrop, Pong>
{
    public async ValueTask<Pong> HandleAsync(
        DEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        DDrop request,
        CancellationToken ct
    )
    {
        await actor.Context.BoundSession.DisconnectAsync(ct);
        return new();
    }
}

public sealed class DRoom(IZLinkSpotContext context) : IZLinkSpot<DActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<DWhereHandler, DActor>();
        Context.Handlers.AddActorPacket<DDropHandler, DActor>();
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actor,
        ZLinkMessage request,
        CancellationToken ct
    ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

    public ValueTask OnJoinedActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(DActor actor, CancellationToken ct) =>
        actor.ObserveDisconnected();

    public ValueTask OnLeaveActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

public sealed class DWhereHandler : IZLinkSpotActorRequestHandler<DRoom, DActor, DWhere, DLocation>
{
    public ValueTask<DLocation> HandleAsync(
        DRoom spot,
        DActor actor,
        IZLinkMessageContext context,
        DWhere request,
        CancellationToken ct
    ) => ValueTask.FromResult(new DLocation(actor.Context.SpotId));
}

public sealed class DDropHandler : IZLinkSpotActorRequestHandler<DRoom, DActor, DDrop, Pong>
{
    public async ValueTask<Pong> HandleAsync(
        DRoom spot,
        DActor actor,
        IZLinkMessageContext context,
        DDrop request,
        CancellationToken ct
    )
    {
        await actor.Context.BoundSession.DisconnectAsync(ct);
        return new();
    }
}

public sealed class DOrderedHandler
    : IZLinkEntrySpotActorRequestHandler<DEntry, DActor, DOrdered, Pong>
{
    public async ValueTask<Pong> HandleAsync(
        DEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        DOrdered request,
        CancellationToken ct
    )
    {
        if (request.Push)
        {
            await actor.Context.BoundSession.Send(new Pong()).Async();
            await actor.Context.BoundSession.DisconnectAsync(ct);
        }
        return new();
    }
}
