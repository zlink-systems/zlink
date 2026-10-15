using Zlink.Framework.Contracts.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task UserSpotConfigureUsesActivationIdentityAfterHostStartup()
    {
        IdentitySubscriptionSpot.ConfiguredContexts.Clear();
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(IdentitySubscriptionSpot),
            includeEntryChannelMembership: true
        );
        try
        {
            Assert.Empty(IdentitySubscriptionSpot.ConfiguredContexts);
            var result = await runtime.GetOrCreateAsync<IdentitySubscriptionSpot>("identity-spot");
            var context = Assert.Single(IdentitySubscriptionSpot.ConfiguredContexts);
            Assert.Equal(result.Spot.SpotId, context.SpotId);
            Assert.Equal(result.Spot.ObjectGeneration, context.ObjectGeneration);
            Assert.Equal(node.RoutingId, context.NodeRid);
            Assert.Equal("entry", context.MeshName);
            Assert.NotEqual(0UL, context.ObjectGeneration);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task UserSpotActivationRejectsMissingRuntimeContext()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            userSpotType: typeof(MissingRuntimeContextSpot)
        );
        try
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.GetOrCreateAsync<MissingRuntimeContextSpot>("missing-context")
            );
            Assert.Contains("must expose the context provided by the runtime", error.Message);
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public Task UserSpotActivationRejectsEmptyTopic() =>
        AssertInvalidRegistrationAsync<EmptyTopicSpot>("topic is required");

    [Fact]
    public Task UserSpotActivationRejectsChannelNonMembership() =>
        AssertInvalidRegistrationAsync<NonMemberSubscriptionSpot>("does not participate");

    [Fact]
    public Task UserSpotActivationRejectsDuplicatePacket() =>
        AssertInvalidRegistrationAsync<NodesAndServicesTests.DuplicatePacketSpot>(
            "SPOT packet handler 'DuplicatePacketMessage' is already registered"
        );

    [Fact]
    public Task UserSpotActivationRejectsDuplicateSubscription() =>
        AssertInvalidRegistrationAsync<NodesAndServicesTests.DuplicateSubscriptionSpot>(
            "SPOT subscription handler",
            "already registered"
        );

    [Fact]
    public Task UserSpotActivationRejectsDuplicateActorHandler() =>
        AssertInvalidRegistrationAsync<NodesAndServicesTests.DuplicateActorSpot>(
            "actor packet",
            "already registered"
        );

    [Fact]
    public async Task EntrySpotConfigureFailureFailsHostStartup()
    {
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await CreateStartedRuntimeAsync(
                new CapturingSpotNode(),
                entrySpotType: typeof(EmptyTopicEntrySpot),
                includeEntryChannelMembership: true
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        Assert.Contains("topic is required", error.Message);
    }

    private sealed class EmptyTopicEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure() =>
            Context.Handlers.AddSubscribe<IdentitySubscriptionHandler>(
                Context.MeshName,
                string.Empty
            );
    }

    private static async Task AssertInvalidRegistrationAsync<TSpot>(params string[] messages)
        where TSpot : class, IZLinkSpot
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            userSpotType: typeof(TSpot),
            includeEntryChannelMembership: true
        );
        try
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.GetOrCreateAsync<TSpot>("invalid-subscription")
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
            foreach (var message in messages)
                Assert.Contains(message, error.Message, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class EmptyTopicSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure() =>
            Context.Handlers.AddSubscribe<IdentitySubscriptionHandler>(
                Context.MeshName,
                string.Empty
            );
    }

    private sealed class NonMemberSubscriptionSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure() =>
            Context.Handlers.AddSubscribe<IdentitySubscriptionHandler>(
                "unjoined-channel",
                Context.SpotId
            );
    }

    private sealed class IdentitySubscriptionSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public static List<IZLinkSpotContext> ConfiguredContexts { get; } = [];

        public IZLinkSpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddSubscribe<IdentitySubscriptionHandler>(
                Context.MeshName,
                Context.SpotId
            );
            ConfiguredContexts.Add(Context);
        }
    }

    private sealed class IdentitySubscriptionHandler
        : IZLinkSpotSubscriptionHandler<IdentitySubscriptionSpot, string>
    {
        public ValueTask HandleAsync(
            IdentitySubscriptionSpot spot,
            string message,
            ZLinkPublishMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class MissingRuntimeContextSpot : IZLinkSpot
    {
        public MissingRuntimeContextSpot(IZLinkSpotContext context) => _ = context;

        public IZLinkSpotContext Context => null!;
    }
}
