using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;

internal static class Program
{
    private static int Main(string[] args)
    {
        _ = FixtureSamples.CreateChannelBuilder();
        _ = FixtureSamples.CreateSpotBuilder();
        _ = FixtureSamples.CreateStreamBuilder();
        _ = FixtureSamples.CreateRegistryBuilder();
        _ = FixtureSamples.CreateMonitoringBuilder();
        _ = FixtureSamples.CreateActorBuilder();

        return 0;
    }
}

internal static class FixtureSamples
{
    public static IHostApplicationBuilder CreateChannelBuilder()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddHandlersFromAssemblyOf<FixtureSendHandler>();

            {
                var mesh = options.AddRouteMesh("orders")
                    .Listen("tcp://127.0.0.1:7201")
                    .SetRoutingId(RoutingId.From("doc-orders"));
                mesh.Channel("orders").Client(); // 논리 channel의 handler namespace를 MeshNode에 등록한다.
                mesh.PeerConnections.Connect(
                    RoutingId.From("doc-orders"),
                    "tcp://127.0.0.1:7201"); // manual peer도 같은 MeshNode admission을 사용한다.
            }

            {
                var channel = options.AddFanoutChannel("orders.events");
                channel.EnablePublisher("tcp://127.0.0.1:7202");
                channel.Connect("tcp://127.0.0.1:7202");
            }
        });
        return builder;
    }

    public static IHostApplicationBuilder CreateSpotBuilder()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddScoped<FixtureSpotTimerHandler>();
        builder.Services.AddScoped<FixtureSpotSubscriptionHandler>();
        builder.Services.AddZLinkFramework(options =>
        {
            {
                var mesh = options.AddRouteMesh("game.stage");
                mesh.Channel("game.stage").Client();
                mesh.Listen("tcp://127.0.0.1:7302");
                mesh.Objects().Server().AddSpotFactory<FixtureStageSpot>(
                    "fixture-stage", factory => factory.RecreateOnRelocation());
            }
        });
        return builder;
    }

    public static IHostApplicationBuilder CreateStreamBuilder()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddScoped<FixtureRawStreamSession>();
        builder.Services.AddZLinkFramework(options =>
        {
            {
                var stream = options.AddStreamNode("stream.raw");
                stream.Bind("tcp://127.0.0.1:7401");
                stream.AddSession<FixtureRawStreamSession>();
            }
        });
        return builder;
    }

    public static IHostApplicationBuilder CreateRegistryBuilder()
    {
        throw new NotSupportedException("The core registry runtime has been removed.");
    }

    public static IHostApplicationBuilder CreateMonitoringBuilder()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {

            {
                var mesh = options.AddRouteMesh("orders")
                    .Listen("tcp://127.0.0.1:7603")
                    .SetRoutingId(RoutingId.From("doc-orders"));
                mesh.Channel("orders").Client();
            }

            {
                var mesh = options.AddRouteMesh("game.stage");
                mesh.Channel("game.stage").Client();
                mesh.Listen("tcp://127.0.0.1:7605");
                mesh.Objects().Server().AddSpotFactory<FixtureStageSpot>(
                    "fixture-stage", factory => factory.RecreateOnRelocation());
            }
        });
        return builder;
    }

    public static IHostApplicationBuilder CreateActorBuilder()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddScoped<FixtureActorPacketSession>();
        builder.Services.AddZLinkFramework(options =>
        {
            {
                var stream = options.AddStreamNode("stream.actor");
                stream.Bind("tcp://127.0.0.1:7701");
                stream.AddSession<FixtureActorPacketSession>();
            }

            {
                var mesh = options.AddRouteMesh("game.stage");
                mesh.Channel("game.stage").Client();
                mesh.Listen("tcp://127.0.0.1:7702");
                mesh.Objects().Server()
                    .AddSpotFactory<FixtureActorSpot>(
                        "fixture-actor-spot", factory => factory.RecreateOnRelocation())
                    .AddActorFactory<FixtureActor, FixtureActorFactory>(
                        "hero", factory => factory.RecreateOnRelocation());
            }
        });
        return builder;
    }
}

internal sealed class FixtureStageSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddSubscribe<FixtureSpotSubscriptionHandler>(
            "game.stage",
            "stage.event");
    }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        _ = await Context.AddTimer<FixtureSpotTimerHandler>(
            "heartbeat",
            TimeSpan.FromSeconds(1),
            cancellationToken: cancellationToken);
    }
}

internal sealed class FixtureSpotTimerHandler
    : IZLinkSpotTimerHandler<FixtureStageSpot>
{
    public async ValueTask HandleAsync(
        FixtureStageSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        _ = tick;
        await spot.Context.Outbound
            .Publish("game.stage", "stage.event", new FixtureSpotEvent(spot.Context.SpotId))
            .Async(cancellationToken);
    }
}

internal sealed class FixtureSpotSubscriptionHandler
    : IZLinkSpotSubscriptionHandler<FixtureStageSpot, FixtureSpotEvent>
{
    public ValueTask HandleAsync(
        FixtureStageSpot spot,
        FixtureSpotEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = spot;
        _ = message;
        _ = context;
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }
}

internal sealed record FixtureSpotEvent(string Value);

internal sealed class FixtureSendHandler
{
    [ZLinkSend]
    public ValueTask HandleAsync(
        FixtureSendCommand command,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = command;
        _ = context;
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }
}

internal sealed record FixtureSendCommand(string Value);

internal sealed class FixtureRawStreamSession(IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        _ = Context;
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        _ = error;
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        _ = payload;
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }
}

internal sealed class FixtureActorSpot(IZLinkSpotContext context) : IZLinkSpot<FixtureActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
    }

    public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var decoded = request.Decode<FixtureActorJoinRequest>();
        _ = actorId;
        await ValueTask.CompletedTask;
        return ZLinkSpotActorJoinResult.Accept(new FixtureActorJoinReply(decoded.RoomId));
    }

    public ValueTask OnJoinedActorAsync(FixtureActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(FixtureActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed record FixtureActorJoinRequest(string RoomId);

internal sealed record FixtureActorJoinReply(string RoomId);

internal sealed class FixtureActorFactory : IZLinkActorFactory<FixtureActor>
{
    public ValueTask<FixtureActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new FixtureActor(context.ActorId, context));
    }
}

internal sealed class FixtureActor(
    string actorId,
    IZLinkActorContext context) : IZLinkActor
{
    public FixtureActorSpot? Spot { get; private set; }
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public void AttachSpot(FixtureActorSpot spot)
    {
        Spot = spot;
    }

    public void DetachSpot(FixtureActorSpot spot)
    {
        if (ReferenceEquals(Spot, spot)) Spot = null;
    }

    public ValueTask OnAttachedAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDetachedAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }
}

internal sealed class FixtureActorPacketSession(
    IZLinkSessionContext context,
    IZLinkActorManager actors) : IZLinkSession
{
    private IZLinkSessionActor? _actor;

    public IZLinkSessionContext Context { get; } = context;

    public async ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        var actor = (await actors.GetOrCreate("fixture", "hero")
                .Async(cancellationToken).ConfigureAwait(false)) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Actor creation was rejected.")
        };

        _actor = await Context.Actors.BindAsync(
            actor,
            cancellationToken);
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        _actor = null;
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        _ = error;
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        var actor = _actor ?? throw new InvalidOperationException("Actor is not bound.");
        await actor.RelayAsync(payload, cancellationToken);
    }
}
