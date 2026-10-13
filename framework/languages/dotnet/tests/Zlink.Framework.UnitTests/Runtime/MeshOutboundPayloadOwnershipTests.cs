using System.Diagnostics;
using Google.Protobuf;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Messaging;
using BytesValue = Google.Protobuf.WellKnownTypes.BytesValue;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class MeshOutboundPayloadOwnershipTests
{
    [Fact]
    public void OneWaySubmissionStateDoesNotAllocateAnOwnerObject()
    {
        for (var index = 0; index < 32; index++)
        {
            var warm = new ZLinkOneWayCallGate("MeshNode send");
            warm.Claim();
        }
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < 1024; index++)
        {
            var gate = new ZLinkOneWayCallGate("MeshNode send");
            gate.Claim();
        }
        Assert.Equal(0L, GC.GetAllocatedBytesForCurrentThread() - before);
    }

    private const string Mesh = "outbound-ownership";
    private const string Channel = "worker";

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Immediate_public_send_waits_for_its_registered_completion_turn(bool channel)
    {
        var received = new ReplyGate();
        using var target = BuildHost(replyGate: received);
        await target.StartAsync();
        var targetNode = target
            .Services.GetRequiredService<ZLinkFrameworkRuntime>()
            .GetMeshNodeRuntime(Mesh);
        using var source = BuildHost(
            endpoint: targetNode.Node.MeshStatus().LocalEndpoint,
            peer: targetNode.Node.RoutingId
        );
        await source.StartAsync();
        using var completionTurn = new BlockingCompletionTurn();
        try
        {
            await WaitForPeerAsync(source, targetNode.Node.RoutingId);
            ZLinkCompletionDispatcher.Shared.Post(completionTurn);
            await completionTurn.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var client = source.Services.GetRequiredService<IZLinkRouteClient>();
            var payload = new BytesValue { Value = ByteString.CopyFrom(new byte[64]) };
            var send = channel
                ? client.SendToChannel(Channel, payload).Async()
                : client.SendToNode(Mesh, targetNode.Node.RoutingId, payload).Async();
            await received.Received.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(send.IsCompleted);
            completionTurn.Release.Set();
            await send.AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            completionTurn.Release.Set();
            await source.StopAsync();
            await target.StopAsync();
        }
    }

    private sealed class BlockingCompletionTurn : ZLinkCompletionDispatcher.WorkItem, IDisposable
    {
        internal TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal ManualResetEventSlim Release { get; } = new();

        internal override void Execute()
        {
            Started.SetResult();
            Release.Wait();
        }

        internal override void Completed(Exception? failure) { }

        public void Dispose() => Release.Dispose();
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task PublicMeshRequest_AdmitsBeforeReply(bool channel)
    {
        var gate = new ReplyGate();
        using var target = BuildHost(replyGate: gate);
        await target.StartAsync();
        var targetNode = target
            .Services.GetRequiredService<ZLinkFrameworkRuntime>()
            .GetMeshNodeRuntime(Mesh);
        using var source = BuildHost(
            endpoint: targetNode.Node.MeshStatus().LocalEndpoint,
            peer: targetNode.Node.RoutingId
        );
        await source.StartAsync();
        try
        {
            await WaitForPeerAsync(source, targetNode.Node.RoutingId);
            var client = source.Services.GetRequiredService<IZLinkRouteClient>();
            var payload = new BytesValue { Value = ByteString.CopyFrom(new byte[64]) };
            var call = channel
                ? client.RequestToChannel(Channel, payload)
                : client.RequestToNode(Mesh, targetNode.Node.RoutingId, payload);
            var request = call.Async<BytesValue>();
            await request.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(request.Reply.IsCompleted);
            gate.Release.SetResult();
            Assert.Equal(4096, (await request).Value.Length);
        }
        finally
        {
            gate.Release.TrySetResult();
            await source.StopAsync();
            await target.StopAsync();
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Public_route_calls_release_original_native_payloads(
        bool channel,
        bool request
    )
    {
        using var tracking = new TrackingProtobufCodec();
        using var target = BuildHost();
        await target.StartAsync();
        var targetNode = target
            .Services.GetRequiredService<ZLinkFrameworkRuntime>()
            .GetMeshNodeRuntime(Mesh);
        var targetEndpoint = Assert.IsType<string>(targetNode.Node.MeshStatus().LocalEndpoint);
        using var source = BuildHost(tracking, targetNode.Node.RoutingId, targetEndpoint);
        await source.StartAsync();
        try
        {
            await WaitForPeerAsync(source, targetNode.Node.RoutingId);
            var client = source.Services.GetRequiredService<IZLinkRouteClient>();
            for (var index = 0; index < 64; index++)
            {
                var payload = new BytesValue
                {
                    Value = ByteString.CopyFrom(new byte[request ? 64 : 4096]),
                };
                if (request)
                {
                    var call = channel
                        ? client.RequestToChannel(Channel, payload)
                        : client.RequestToNode(Mesh, targetNode.Node.RoutingId, payload);
                    var reply = await call.Async<BytesValue>();
                    Assert.Equal(4096, reply.Value.Length);
                }
                else
                {
                    var call = channel
                        ? client.SendToChannel(Channel, payload)
                        : client.SendToNode(Mesh, targetNode.Node.RoutingId, payload);
                    await call.Async();
                }
                Assert.Equal(1, tracking.Owners[index].RefCount);
            }
        }
        finally
        {
            await source.StopAsync();
            await target.StopAsync();
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Cancellation_after_encode_releases_original_native_payload(
        bool channel,
        bool request
    )
    {
        using var cancellation = new CancellationTokenSource();
        using var tracking = new TrackingProtobufCodec(cancellation.Cancel);
        using var target = BuildHost();
        await target.StartAsync();
        var targetNode = target
            .Services.GetRequiredService<ZLinkFrameworkRuntime>()
            .GetMeshNodeRuntime(Mesh);
        var targetEndpoint = Assert.IsType<string>(targetNode.Node.MeshStatus().LocalEndpoint);
        using var source = BuildHost(tracking, targetNode.Node.RoutingId, targetEndpoint);
        await source.StartAsync();
        try
        {
            await WaitForPeerAsync(source, targetNode.Node.RoutingId);
            var client = source.Services.GetRequiredService<IZLinkRouteClient>();
            var payload = new BytesValue { Value = ByteString.CopyFrom(new byte[4096]) };
            if (request)
            {
                var call = channel
                    ? client.RequestToChannel(Channel, payload)
                    : client.RequestToNode(Mesh, targetNode.Node.RoutingId, payload);
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                    call.Async<BytesValue>(cancellation.Token).AsTask()
                );
            }
            else
            {
                var call = channel
                    ? client.SendToChannel(Channel, payload)
                    : client.SendToNode(Mesh, targetNode.Node.RoutingId, payload);
                await call.Async();
            }
            Assert.Equal(1, Assert.Single(tracking.Owners).RefCount);
        }
        finally
        {
            await source.StopAsync();
            await target.StopAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Channel_without_target_releases_original_native_payload(bool request)
    {
        using var tracking = new TrackingProtobufCodec();
        using var source = BuildHost(tracking);
        await source.StartAsync();
        try
        {
            var client = source.Services.GetRequiredService<IZLinkRouteClient>();
            var payload = new BytesValue { Value = ByteString.CopyFrom(new byte[4096]) };
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            {
                if (request)
                    await client.RequestToChannel(Channel, payload).Async<BytesValue>();
                else
                    await client.SendToChannel(Channel, payload).Async();
            });
            //  A channel with nothing to select ends as Unavailable, not
            //  NotFound: the send path is registered and its connection is
            //  there, only the eligible set is empty
            //  (06-framework-api "no eligible select-one member"). The payload
            //  ownership this test guards is unchanged either way.
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            Assert.Equal(1, Assert.Single(tracking.Owners).RefCount);
        }
        finally
        {
            await source.StopAsync();
        }
    }

    [Fact]
    public async Task Actor_send_without_peer_releases_original_native_payload()
    {
        using var source = BuildHost();
        await source.StartAsync();
        try
        {
            var node = source
                .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                .GetMeshNodeRuntime(Mesh);
            var payload = Message.Allocate(4096);
            using var witness = payload.Copy();
            var result = await node.SendToActorAsync(
                new Zlink.Framework.Runtime.Backend.Contracts.ZLinkBackendActorRef(
                    RoutingId.From("missing-peer"),
                    "actor",
                    1
                ),
                [payload],
                CancellationToken.None
            );
            Assert.Equal(ZLinkOneWaySubmitStatus.RouteNotConnected, result.Status);
            Assert.Equal(1, witness.RefCount);
        }
        finally
        {
            await source.StopAsync();
        }
    }

    private static IHost BuildHost(
        TrackingProtobufCodec? tracking = null,
        RoutingId peer = default,
        string? endpoint = null,
        ReplyGate? replyGate = null
    )
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        if (replyGate is not null)
            builder.Services.AddSingleton(replyGate);
        builder.Services.AddZLinkFramework(options =>
        {
            options.Codecs.Use(tracking is null ? ZLinkProtobufCodec.Default : tracking);
            var mesh = options
                .AddRouteMesh(Mesh)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingId(RoutingId.From(Guid.NewGuid().ToString("N")));
            mesh.Channel(Channel)
                .Server()
                .AddSendHandler<ProbeHandler, BytesValue>(nameof(BytesValue))
                .AddRequestHandler<ProbeHandler, BytesValue, BytesValue>(nameof(BytesValue));
            mesh.AddRouteSendHandler<ProbeHandler, BytesValue>(nameof(BytesValue))
                .AddRouteRequestHandler<ProbeHandler, BytesValue, BytesValue>(nameof(BytesValue));
            if (endpoint is not null)
                mesh.PeerConnections.Connect(peer, endpoint);
        });
        return builder.Build();
    }

    private static async Task WaitForPeerAsync(IHost host, RoutingId peer)
    {
        var runtime = host.Services.GetRequiredService<IZLinkRouteMeshRuntime>();
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 3;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (
                runtime
                    .GetStatus(Mesh)
                    .Peers.Any(candidate =>
                        candidate.NodeRid == peer && candidate.State == ZLinkPeerState.Ready
                    )
            )
                return;
            await Task.Delay(10);
        }
        Assert.Fail("The ownership test peer was not admitted.");
    }

    public sealed class ReplyGate
    {
        public TaskCompletionSource Received { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    public sealed class ProbeHandler(ReplyGate? gate = null)
        : IZLinkRouteSendHandler<BytesValue>,
            IZLinkRouteRequestHandler<BytesValue, BytesValue>,
            IZLinkSendHandler<BytesValue>,
            IZLinkRequestHandler<BytesValue, BytesValue>
    {
        ValueTask IZLinkRouteSendHandler<BytesValue>.HandleAsync(
            BytesValue message,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken
        ) => SendAsync();

        ValueTask<BytesValue> IZLinkRouteRequestHandler<BytesValue, BytesValue>.HandleAsync(
            BytesValue request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken
        ) => ReplyAsync();

        public ValueTask HandleAsync(
            BytesValue message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => SendAsync();

        private ValueTask SendAsync()
        {
            gate?.Received.TrySetResult();
            return ValueTask.CompletedTask;
        }

        ValueTask<BytesValue> IZLinkRequestHandler<BytesValue, BytesValue>.HandleAsync(
            BytesValue request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ReplyAsync();

        private async ValueTask<BytesValue> ReplyAsync()
        {
            if (gate is not null)
                await gate.Release.Task;
            return new BytesValue { Value = ByteString.CopyFrom(new byte[4096]) };
        }
    }

    private sealed class TrackingProtobufCodec(Action? afterEncode = null)
        : IZLinkCodecExtension,
            IZLinkMessageSerializer,
            IZLinkMessagePartSerializer,
            IZLinkMessageSpanDeserializer,
            IDisposable
    {
        private readonly IZLinkMessageSerializer _serializer = Serializer();
        internal List<Message> Owners { get; } = [];

        public void Register(IZLinkCodecRegistrar codecs) =>
            codecs.AddSerializer(
                "application/x-protobuf",
                this,
                type => typeof(IMessage).IsAssignableFrom(type)
            );

        public Message SerializePart(object value, Type type)
        {
            var part = ((IZLinkMessagePartSerializer)_serializer).SerializePart(value, type);
            // A native reference-count witness survives wrapper pooling. Reading
            // the submitted wrapper after handoff would violate binding ownership.
            Owners.Add(part.Copy());
            afterEncode?.Invoke();
            return part;
        }

        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            _serializer.Serialize(value, type);

        public object? Deserialize(ZLinkEncodedPayload payload, Type type) =>
            _serializer.Deserialize(payload, type);

        public object? Deserialize(ReadOnlySpan<byte> payload, Type type) =>
            ((IZLinkMessageSpanDeserializer)_serializer).Deserialize(payload, type);

        public void Dispose()
        {
            foreach (var owner in Owners)
                owner.Dispose();
        }

        private static IZLinkMessageSerializer Serializer()
        {
            var codecs = new ZLinkCodecRegistryBuilder();
            codecs.Use(ZLinkProtobufCodec.Default);
            Assert.True(codecs.TryGetSerializer("application/x-protobuf", out var serializer));
            return serializer;
        }
    }
}
