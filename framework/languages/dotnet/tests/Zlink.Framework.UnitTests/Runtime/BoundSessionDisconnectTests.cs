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
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.BoundSessionDisconnect;

public sealed class BoundSessionDisconnectTests
{
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
            await Context.Actors.Bound.Single().RelayAsync(message);
    }
}

public sealed class DEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<DActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<DMoveHandler, DActor>();
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
