using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.BoundSessionDisconnect;

public sealed class OrderedBoundSessionSendTests
{
    [Fact]
    public async Task Remote_bound_sends_keep_session_order_across_node_claims()
    {
        var locations = new ZLinkInMemoryProviderLocationStore();
        var probe = new OrderedSendProbe();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(probe);
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddLocationStore(locations);
            var objects = options
                .AddRouteMesh("disconnect")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<OrderedSendEntry>();
            objects.AddActorFactory<DActor, DFactory>(
                "actor",
                factory => factory.DisableRelocation()
            );
        });
        var traceDirectory = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY");
        using var flow = traceDirectory is null
            ? null
            : new TestHostMessageFlowListener(
                Path.Combine(traceDirectory, "ordered-bound-send.flow")
            );
        using var owner = builder.Build();
        await owner.StartAsync();
        using var gateway = await BoundSessionDisconnectTests.StartGatewayAsync(owner, locations);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var token = deadline.Token;
        var runtime = owner.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        var mesh = runtime.GetMeshNodeRuntime("disconnect");
        var runner = new ZLinkRuntimeTaskRunner(
            runtime.ErrorSink,
            runtime.ShutdownToken,
            runtime.ExecutionOwner
        );
        var dispatcher = Assert.IsType<ZLinkMeshNodeRouteDispatcher>(
            ZLinkMeshNodeRouteDispatcher.Create(
                owner.Services,
                runtime.Registration,
                mesh.Registration,
                runtime,
                runner
            )
        );
        var firstClaim = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var firstDispatched = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var releaseClaim = new ManualResetEventSlim();
        mesh.Node.OnNodeRoute(
            async (records, cancellationToken) =>
            {
                var record = Assert.Single(records);
                var header = record.ApplicationPayloadView is { } payload
                    ? ZLinkEnvelopeCodec.DecodeHeader(payload)
                    : ZLinkEnvelopeCodec.DecodeHeader(record.Parts);
                if (
                    header.MessageName != ZLinkRemoteActorFrameProtocol.PacketName
                    || !firstClaim.TrySetResult()
                )
                {
                    await dispatcher.DispatchBatchAsync(records, cancellationToken);
                    return;
                }

                // The first Node claim has received its record. Hold it before it
                // registers its Actor relay tail, so another claim can register first.
                try
                {
                    releaseClaim.Wait(token);
                    await dispatcher.DispatchBatchAsync(records, cancellationToken);
                }
                finally
                {
                    firstDispatched.TrySetResult();
                }
            },
            runner
        );

        try
        {
            var actorId = $"ordered-send:{Guid.NewGuid()}";
            var created = Assert.IsType<ZLinkActorCreateResult.Created>(
                await owner
                    .Services.GetRequiredService<IZLinkActorManager>()
                    .GetOrCreate(actorId, "actor")
                    .InMesh("disconnect")
                    .Async(token)
            );
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
            await actor.Send(new OrderedSend(1)).Async();
            await Task.WhenAny(
                    firstClaim.Task,
                    probe.Handled.Reader.WaitToReadAsync(token).AsTask()
                )
                .WaitAsync(token);
            await actor.Send(new OrderedSend(2)).Async();

            var first = await probe.Handled.Reader.ReadAsync(token);
            releaseClaim.Set();
            if (firstClaim.Task.IsCompleted)
                await firstDispatched.Task.WaitAsync(token);

            Assert.Equal(1, first);
            Assert.Equal(2, await probe.Handled.Reader.ReadAsync(token));

            Assert.True(runtime.TryGetActorBoundSessionForOutbound(actorId, out var binding));
            Assert.Equal(2UL, binding.AcceptedHighWater);
            var gatewayRuntime = gateway.Services.GetRequiredService<ZLinkFrameworkRuntime>();
            var source = gatewayRuntime.GetMeshNodeRuntime("disconnect").LocalRequestSource;
            var staleHeader = ZLinkStreamProtocolDefaults.EncodeHeader(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Json,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    nameof(OrderedSend),
                    ZlinkStreamMetadata.Empty
                )
            );
            await gatewayRuntime.RelayRemoteActorFrameAsync(
                created.Actor.MeshName,
                created.Actor.ToBackend(),
                binding.TargetNodeGeneration,
                binding.AuthorityOwnerGeneration,
                binding.OwnerLeaseGeneration,
                source.NodeRid,
                binding.SessionRid,
                new ZLinkBackendActorRouteContext(
                    new MeshOperationId(1, 1),
                    0,
                    binding.TargetNodeGeneration,
                    binding.AuthorityOwnerGeneration,
                    binding.OwnerLeaseGeneration,
                    0,
                    0,
                    IsBoundSessionRoute: true
                ),
                source.NodeGeneration,
                source,
                ZLinkActorBoundSessionHandoffMetadata.Encode(
                    new ZLinkActorBoundSessionHandoffFence(
                        actorId,
                        binding.ObjectGeneration,
                        binding.SessionRid,
                        binding.BindingToken,
                        binding.BindingGeneration,
                        SessionSequence: 1
                    )
                ),
                staleHeader.ToArray(),
                ZLinkEnvelopeCodec.EncodeJsonBytes(new OrderedSend(99), typeof(OrderedSend)),
                token
            );
            // This send follows the stale relay on the same connection and Actor tail.
            await actor.Send(new OrderedSend(3)).Async();
            Assert.Equal(3, await probe.Handled.Reader.ReadAsync(token));
        }
        finally
        {
            releaseClaim.Set();
            await gateway.StopAsync();
            await owner.StopAsync();
            await runner.StopAsync();
        }
    }
}

public sealed record OrderedSend(int Number);

public sealed class OrderedSendProbe
{
    internal Channel<int> Handled { get; } = Channel.CreateUnbounded<int>();
}

public sealed class OrderedSendEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<DActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() => Context.Handlers.AddActorPacket<OrderedSendHandler, DActor>();

    public ValueTask OnJoinedActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(DActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

public sealed class OrderedSendHandler(OrderedSendProbe probe)
    : IZLinkEntrySpotActorSendHandler<OrderedSendEntry, DActor, OrderedSend>
{
    public ValueTask HandleAsync(
        OrderedSendEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        OrderedSend message,
        CancellationToken ct
    )
    {
        probe.Handled.Writer.TryWrite(message.Number);
        return ValueTask.CompletedTask;
    }
}
