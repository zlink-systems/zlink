using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.BoundSessionDisconnect;

public sealed class BoundSessionRelayPushOrderTests
{
    [Fact]
    public Task Remote_bound_session_push_reaches_client_while_request_handler_holds_reply() =>
        RunRelayPushAsync(discoverAfterAdmission: false);

    [Fact]
    public Task Discovery_preserves_the_inbound_route_for_remote_bind_and_push() =>
        RunRelayPushAsync(discoverAfterAdmission: true);

    private static async Task RunRelayPushAsync(bool discoverAfterAdmission)
    {
        var locations = new ZLinkInMemoryProviderLocationStore();
        const string ownerRidPrefix = "a";
        var discovery = discoverAfterAdmission
            ? new DeferredDiscovery(locations, ownerRidPrefix)
            : null;
        var probe = new RelayPushProbe();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(probe);
        builder.Services.AddZLinkFramework(o =>
        {
            o.DisableImplicitHandlerAutoRegistration();
            o.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            o.AddLocationStore(discovery is null ? locations : discovery);
            var mesh = o.AddRouteMesh("disconnect").Listen("tcp://127.0.0.1:0");
            if (discovery is not null)
                mesh.SetRoutingIdPrefix(ownerRidPrefix);
            var objects = mesh.Objects().Server();
            objects.AddEntrySpot<RelayPushEntry>();
            objects.AddActorFactory<DActor, DFactory>("actor", f => f.DisableRelocation());
        });
        if (discovery is not null)
        {
            // Only discovery should scan the owner's store before the client sends.
            builder.Services.RemoveAll<IZLinkLocationDescriptorQuery>();
        }
        var traceDirectory = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY");
        using var flow = traceDirectory is null
            ? null
            : new TestHostMessageFlowListener(
                Path.Combine(traceDirectory, "relay-push-order.flow")
            );
        using var owner = builder.Build();
        await owner.StartAsync();
        using var gateway = await BoundSessionDisconnectTests.StartGatewayAsync(owner, locations);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        try
        {
            var token = deadline.Token;
            if (discovery is not null)
            {
                var runtime = owner.Services.GetRequiredService<IZLinkRouteMeshRuntime>();
                await foreach (var update in runtime.ObserveAsync("disconnect", token))
                    if (update.Status.ReadyPeerCount == 1)
                        break;

                // The public topology groups peers by RID. Compare the underlying
                // peer snapshot identity to detect an outbound intent for this RID.
                var node = owner
                    .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                    .GetSpotNodeRuntime("disconnect")
                    .Node;
                var inbound = Assert.Single(node.MeshPeers());
                await discovery.RevealAndWaitForReconcileAsync(token);
                Assert.Equal(
                    inbound.ConnectionIntentId,
                    Assert.Single(node.MeshPeers()).ConnectionIntentId
                );
            }
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

    private sealed class DeferredDiscovery(IZLinkLocationStore inner, string ownerRidPrefix)
        : IZLinkLocationStore
    {
        private readonly string _ownerKeyPrefix = ZLinkProviderLocationRepository
            .MeshKey("disconnect", RoutingId.From(ownerRidPrefix + "-"))
            .Value;
        private readonly TaskCompletionSource _reconciled = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private volatile bool _revealed;
        private int _visibleScans;

        internal Task RevealAndWaitForReconcileAsync(CancellationToken token)
        {
            _revealed = true;
            return _reconciled.Task.WaitAsync(token);
        }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(ZLinkStoreKey key, CancellationToken ct) =>
            inner.ReadAsync(key, ct);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken ct
        ) => inner.WriteAsync(request, ct);

        public async ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken ct
        )
        {
            var result = await inner.ScanAsync(request, ct);
            if (
                request.Prefix != "mesh-node\0disconnect\0"
                || result is not ZLinkStoreScanResult.Page page
                || page.Value.Items.Count != 2
            )
                return result;
            if (!_revealed)
                return new ZLinkStoreScanResult.Page(
                    page.Value with
                    {
                        Items = page
                            .Value.Items.Where(item =>
                                item.Key.Value.StartsWith(_ownerKeyPrefix, StringComparison.Ordinal)
                            )
                            .ToArray(),
                    }
                );

            // The owner's next descriptor scan follows the reconcile that saw the row.
            if (Interlocked.Increment(ref _visibleScans) == 2)
                _reconciled.TrySetResult();
            return result;
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
