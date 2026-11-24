using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.BoundSessionDisconnect;

public sealed class OrderedBoundSessionPushTests
{
    [Fact]
    public async Task Remote_bound_session_pushes_reach_connector_in_submission_order()
    {
        var locations = new ZLinkInMemoryProviderLocationStore();
        var ownerBuilder = Host.CreateApplicationBuilder();
        ownerBuilder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddLocationStore(locations);
            var objects = options
                .AddRouteMesh("disconnect")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<OrderedPushEntry>();
            objects.AddActorFactory<DActor, DFactory>(
                "actor",
                factory => factory.DisableRelocation()
            );
        });
        var traceDirectory = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY");
        using var flow = traceDirectory is null
            ? null
            : new TestHostMessageFlowListener(
                Path.Combine(traceDirectory, "ordered-bound-session-push.flow")
            );
        using var owner = ownerBuilder.Build();
        await owner.StartAsync();
        try
        {
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var token = deadline.Token;
            var actorId = $"ordered-push:{Guid.NewGuid()}";
            await owner
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, "actor")
                .InMesh("disconnect")
                .Async(token);
            using var gateway = await BoundSessionDisconnectTests.StartGatewayAsync(
                owner,
                locations
            );
            try
            {
                var endpoint = gateway
                    .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                    .GetListenerStatus(ZLinkListenerKind.Stream, "clients")
                    .Endpoint;
                await using var client = ZlinkStreamConnectorFactory.Create(
                    new()
                    {
                        Endpoint = new(endpoint),
                        DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    }
                );
                await client.Connect.Async().AsTask().WaitAsync(token);
                await client.Request(new DLogin(actorId)).Async<Pong>().AsTask().WaitAsync(token);
                var actor = Assert.IsAssignableFrom<IZlinkStreamActor>(client.Actor(actorId));
                var received = Channel.CreateUnbounded<string>();
                using var first = actor.On<FirstOrderedPush>(
                    (_, _) =>
                    {
                        received.Writer.TryWrite(nameof(FirstOrderedPush));
                        return ValueTask.CompletedTask;
                    }
                );
                using var second = actor.On<SecondOrderedPush>(
                    (_, _) =>
                    {
                        received.Writer.TryWrite(nameof(SecondOrderedPush));
                        return ValueTask.CompletedTask;
                    }
                );
                var actors = owner.Services.GetRequiredService<IZLinkActorClient>();
                for (var index = 0; index < 40; index++)
                {
                    await actors.RequestToActor(actorId, new OrderedPushRequest()).Async<Pong>();
                    var actual = new[]
                    {
                        await received.Reader.ReadAsync(token),
                        await received.Reader.ReadAsync(token),
                    };
                    Assert.Equal(
                        new[] { nameof(FirstOrderedPush), nameof(SecondOrderedPush) },
                        actual
                    );
                }
            }
            finally
            {
                await gateway.StopAsync();
            }
        }
        finally
        {
            await owner.StopAsync();
        }
    }
}

public sealed record OrderedPushRequest;

public sealed record FirstOrderedPush;

public sealed record SecondOrderedPush;

public sealed class OrderedPushEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<DActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() => Context.Handlers.AddActorPacket<OrderedPushHandler, DActor>();

    public ValueTask OnJoinedActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

public sealed class OrderedPushHandler
    : IZLinkEntrySpotActorRequestHandler<OrderedPushEntry, DActor, OrderedPushRequest, Pong>
{
    public async ValueTask<Pong> HandleAsync(
        OrderedPushEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        OrderedPushRequest request,
        CancellationToken ct
    )
    {
        await actor.Context.BoundSession.Send(new FirstOrderedPush()).Async();
        await actor.Context.BoundSession.Send(new SecondOrderedPush()).Async();
        return new Pong();
    }
}
