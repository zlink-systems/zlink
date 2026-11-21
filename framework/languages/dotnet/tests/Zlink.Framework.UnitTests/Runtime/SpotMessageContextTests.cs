using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotMessageContextTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Spot_send_and_request_deliver_metadata_to_the_owner(bool remote)
    {
        var received = new TaskCompletionSource<string?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        const string spotType = "metadata-probe";
        await using var owner = await SpotCloseHost.StartAsync(
            registerInstanceFactory: false,
            registerUserSpotFactory: false,
            configureServer: (services, server) =>
            {
                services.AddSingleton(received);
                server.AddSpotFactory<MetadataSpot>(
                    spotType,
                    factory => factory.DisableRelocation()
                );
            }
        );
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var spot = (
            await owner.Manager.Create(spotType).InMesh(SpotCloseHost.MeshName).Async()
        ).Spot;
        await using var peer = remote
            ? await SpotCloseHost.StartAsync(owner.Store, clientOnly: true)
            : null;
        if (peer is not null)
            await peer.WaitForPeerAsync(owner, cancellation.Token);
        var client = (peer ?? owner).Runtime.Services.GetRequiredService<IZLinkSpotClient>();

        await client
            .SendToSpot(spot.SpotId, new MetadataPacket())
            .Metadata("trace-id", "first")
            .Metadata("trace-id", "sent")
            .Async();
        Assert.Equal("sent", await received.Task.WaitAsync(cancellation.Token));

        var reply = await client
            .RequestToSpot(spot.SpotId, new MetadataRequest())
            .Metadata("trace-id", "requested")
            .Async<MetadataReply>(cancellation.Token);
        Assert.Equal("requested", reply.Value);
    }

    public sealed record MetadataPacket;

    public sealed record MetadataRequest;

    public sealed record MetadataReply(string? Value);

    public sealed class MetadataSpot(IZLinkSpotContext context) : IZLinkSpot<IZLinkActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    public sealed class MetadataPacketHandler(TaskCompletionSource<string?> received)
        : IZLinkSpotPacketHandler<MetadataSpot, MetadataPacket>
    {
        public ValueTask HandleAsync(
            MetadataSpot spot,
            IZLinkMessageContext context,
            MetadataPacket message,
            CancellationToken cancellationToken
        )
        {
            received.SetResult(context.Metadata.Find("trace-id"));
            return ValueTask.CompletedTask;
        }
    }

    public sealed class MetadataRequestHandler
        : IZLinkSpotRequestHandler<MetadataSpot, MetadataRequest, MetadataReply>
    {
        public ValueTask<MetadataReply> HandleAsync(
            MetadataSpot spot,
            IZLinkMessageContext context,
            MetadataRequest request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new MetadataReply(context.Metadata.Find("trace-id")));
    }
}
