using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.BoundSessionDisconnect;

public sealed class BoundSessionRelayPushOrderTests
{
    [Fact]
    public async Task Remote_bound_session_push_reaches_client_while_request_handler_holds_reply()
    {
        var locations = new ZLinkInMemoryProviderLocationStore();
        var probe = new RelayPushProbe();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(probe);
        builder.Services.AddZLinkFramework(o =>
        {
            o.DisableImplicitHandlerAutoRegistration();
            o.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            o.AddLocationStore(locations);
            var objects = o.AddRouteMesh("disconnect")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<RelayPushEntry>();
            objects.AddActorFactory<DActor, DFactory>("actor", f => f.DisableRelocation());
        });
        var traceDirectory = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY");
        using var flow = traceDirectory is null
            ? null
            : new TestHostMessageFlowListener(
                Path.Combine(traceDirectory, "relay-push-order.flow")
            );
        using var owner = builder.Build();
        await owner.StartAsync();
        using var gateway = BoundSessionDisconnectTests.BuildGateway(owner, locations);
        await gateway.StartAsync();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        try
        {
            var token = deadline.Token;
            var actorId = $"relay-push:{Guid.NewGuid()}";
            await owner
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, "actor")
                .InMesh("disconnect")
                .Async(token);
            var endpoint = gateway
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .GetListenerStatus(ZLinkListenerKind.Stream, "clients")
                .Endpoint;
            await using var client = ZlinkStreamConnectorFactory.Create(
                new() { Endpoint = new(endpoint), DispatchMode = ZlinkStreamDispatchMode.Immediate }
            );
            await client.Connect.Async().AsTask().WaitAsync(token);
            await client.Request(new DLogin(actorId)).Async<Pong>().AsTask().WaitAsync(token);
            var actor = Assert.IsAssignableFrom<IZlinkStreamActor>(client.Actor(actorId));
            var push = new TaskCompletionSource<RelayPushNotice>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var subscription = actor.On<RelayPushNotice>(
                (message, _) =>
                {
                    push.TrySetResult(message.Payload);
                    return ValueTask.CompletedTask;
                }
            );
            var reply = actor.Request(new RelayPushRequest("probe")).Async<Pong>().AsTask();
            try
            {
                await probe.PushSubmitted.Task.WaitAsync(token);
                Assert.Same(push.Task, await Task.WhenAny(push.Task, reply).WaitAsync(token));
                Assert.Equal("probe", (await push.Task).Value);
                Assert.False(reply.IsCompleted);
            }
            finally
            {
                probe.ReleaseReply.TrySetResult();
            }
            Assert.NotNull(await reply.WaitAsync(token));
        }
        finally
        {
            await gateway.StopAsync();
            await owner.StopAsync();
        }
    }
}

public sealed record RelayPushRequest(string Value);

public sealed record RelayPushNotice(string Value);

public sealed class RelayPushProbe
{
    public TaskCompletionSource PushSubmitted { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource ReleaseReply { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

public sealed class RelayPushEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<DActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() => Context.Handlers.AddActorPacket<RelayPushHandler, DActor>();

    public ValueTask OnJoinedActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

public sealed class RelayPushHandler(RelayPushProbe probe)
    : IZLinkEntrySpotActorRequestHandler<RelayPushEntry, DActor, RelayPushRequest, Pong>
{
    public async ValueTask<Pong> HandleAsync(
        RelayPushEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        RelayPushRequest request,
        CancellationToken ct
    )
    {
        await actor.Context.BoundSession.Send(new RelayPushNotice(request.Value)).Async();
        probe.PushSubmitted.TrySetResult();
        await probe.ReleaseReply.Task.WaitAsync(ct);
        return new Pong();
    }
}
