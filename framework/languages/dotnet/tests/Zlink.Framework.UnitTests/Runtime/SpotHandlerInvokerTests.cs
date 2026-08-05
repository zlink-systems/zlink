using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Handlers;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotHandlerInvokerTests
{
    [Fact]
    public async Task InvokePacketAsync_Reuses_Unregistered_Handler_And_Disposes_It_Once()
    {
        var lifetime = new HandlerLifetime();
        using var provider = new ServiceCollection()
            .AddSingleton(lifetime)
            .BuildServiceProvider();
        var handlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var spot = new MemberLifecycleSpot();
        var descriptor = ZLinkSpotDescriptorFactory.CreatePacketDescriptor(
            typeof(DisposablePacketHandler),
            typeof(MemberLifecycleSpot));
        var invoker = new ZLinkSpotHandlerInvoker(
            handlerInstances,
            spot,
            new ZLinkCodecRegistryBuilder(),
            ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec());

        await invoker.InvokePacketAsync(descriptor, new HandlerMessage(), CancellationToken.None);
        await invoker.InvokePacketAsync(descriptor, new HandlerMessage(), CancellationToken.None);

        Assert.Equal(2, lifetime.Invocations.Count);
        Assert.Same(lifetime.Invocations[0], lifetime.Invocations[1]);
        await handlerInstances.DisposeAsync();
        await handlerInstances.DisposeAsync();
        Assert.Equal(1, lifetime.DisposeCount);
    }

    [Fact]
    public async Task ExplicitlyRegistered_Handler_Type_Is_Still_Created_By_Activation_Owner()
    {
        var lifetime = new HandlerLifetime();
        await using var provider = new ServiceCollection()
            .AddSingleton(lifetime)
            .AddScoped<DisposablePacketHandler>()
            .BuildServiceProvider();
        var scope = provider.CreateAsyncScope();
        var handlerInstances = new ZLinkScopedHandlerInstanceOwner(scope.ServiceProvider);

        var first = handlerInstances.Resolve<DisposablePacketHandler>();
        var second = handlerInstances.Resolve<DisposablePacketHandler>();

        Assert.Same(first, second);
        await handlerInstances.DisposeAsync();
        Assert.Equal(1, lifetime.DisposeCount);
        await scope.DisposeAsync();
        Assert.Equal(1, lifetime.DisposeCount);
    }

    [Fact]
    public async Task InitializationFailure_Cleans_Up_AlreadyCreated_Fallback_Handler()
    {
        var lifetime = new HandlerLifetime();
        using var provider = new ServiceCollection()
            .AddSingleton(lifetime)
            .BuildServiceProvider();
        var handlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        _ = handlerInstances.Resolve<DisposablePacketHandler>();

        Assert.Throws<InvalidOperationException>(() => handlerInstances.Resolve<FailingHandler>());
        await handlerInstances.DisposeAsync();

        Assert.Equal(1, lifetime.DisposeCount);
    }

    [Fact]
    public async Task InvokeActorLifecycleAsync_Uses_CurrentSpotInstance_WhenHandlerTypeIsSpotType()
    {
        var spot = new MemberLifecycleSpot();
        var actor = new MemberLifecycleActor("player-1");
        var descriptor = ZLinkSpotActorAttributedDescriptorFactory
            .CreateSpotLifecycleDescriptors(ZLinkSpotActorHandlerSurface.UserSpot, typeof(MemberLifecycleSpot))
            .Single(item => item.Joined is not null)
            .Joined!;

        using var provider = new ServiceCollection().BuildServiceProvider();
        await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var invoker = new ZLinkSpotHandlerInvoker(
            handlerInstances,
            spot,
            new ZLinkCodecRegistryBuilder(),
            ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec());

        await invoker.InvokeActorLifecycleAsync(
            descriptor,
            actor,
            CancellationToken.None);

        Assert.Equal("player-1", spot.JoinedActorId);
    }

    [Fact]
    public async Task InvokeActorPacketAsync_Reuses_PerActor_Handler_Not_PerSpot_Handler()
    {
        var lifetime = new ActorHandlerLifetime();
        await using var provider = new ServiceCollection()
            .AddSingleton(lifetime)
            .AddScoped<ActorScopedDependency>()
            .BuildServiceProvider();
        await using var spotHandlerInstances =
            new ZLinkScopedHandlerInstanceOwner(provider);
        var firstState = new ZLinkActorRuntimeState("player-1", services: provider);
        var secondState = new ZLinkActorRuntimeState("player-2", services: provider);
        var states = new Dictionary<string, ZLinkActorRuntimeState>
        {
            ["player-1"] = firstState,
            ["player-2"] = secondState
        };
        var spot = new MemberLifecycleSpot();
        var descriptor = ZLinkSpotActorInterfaceDescriptorFactory
            .CreateInferredDescriptors(
                ZLinkSpotActorHandlerSurface.UserSpot,
                typeof(MemberLifecycleSpot),
                typeof(ActorPacketHandler),
                packetName: null)
            .Single()
            .Packet!;
        var invoker = new ZLinkSpotHandlerInvoker(
            spotHandlerInstances,
            spot,
            "test-mesh",
            new ZLinkCodecRegistryBuilder(),
            ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec(),
            actorHandlerInstances: actor => states[actor.Context.ActorId].HandlerInstances);
        var first = new MemberLifecycleActor("player-1");
        var second = new MemberLifecycleActor("player-2");

        await InvokeAsync(first, "first");
        await InvokeAsync(first, "second");
        await InvokeAsync(second, "third");

        Assert.Equal(3, lifetime.Invocations.Count);
        Assert.Same(lifetime.Invocations[0].Handler, lifetime.Invocations[1].Handler);
        Assert.Same(lifetime.Invocations[0].Dependency, lifetime.Invocations[1].Dependency);
        Assert.NotSame(lifetime.Invocations[0].Handler, lifetime.Invocations[2].Handler);
        Assert.NotSame(lifetime.Invocations[0].Dependency, lifetime.Invocations[2].Dependency);

        await firstState.DisposeHandlerActivationAsync();
        Assert.Equal(1, lifetime.Invocations[0].Handler.DisposeCount);
        Assert.Equal(0, lifetime.Invocations[2].Handler.DisposeCount);
        await secondState.DisposeHandlerActivationAsync();
        Assert.Equal(1, lifetime.Invocations[2].Handler.DisposeCount);

        async ValueTask InvokeAsync(MemberLifecycleActor actor, string value)
        {
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                null,
                nameof(ActorHandlerMessage),
                ZlinkStreamMetadata.Empty);
            using var body = Message.From(
                ZLinkEnvelopeCodec.EncodeJsonBytes(
                    new ActorHandlerMessage(value),
                    typeof(ActorHandlerMessage)));
            await invoker.InvokeActorPacketAsync(
                descriptor,
                actor,
                header,
                body,
                CancellationToken.None);
        }
    }

    [Fact]
    public void CreateSpotLifecycleDescriptors_Uses_onLeaveActor_Hook()
    {
        var descriptor = ZLinkSpotActorAttributedDescriptorFactory
            .CreateSpotLifecycleDescriptors(ZLinkSpotActorHandlerSurface.UserSpot, typeof(MemberLifecycleSpot))
            .Single(item => item.Left is not null)
            .Left!;

        Assert.Equal(typeof(MemberLifecycleSpot), descriptor.HandlerType);
        Assert.Equal(typeof(MemberLifecycleActor), descriptor.ActorType);
        Assert.False(descriptor.PassSpotArgument);
    }

    [Fact]
    public void CreateSpotLifecycleDescriptors_Accepts_Exact_EntryActorCreation_Hook()
    {
        var descriptor = ZLinkSpotActorAttributedDescriptorFactory
            .CreateSpotLifecycleDescriptors(
                ZLinkSpotActorHandlerSurface.EntrySpot,
                typeof(EntryLifecycleSpot))
            .Single(item => item.Created is not null)
            .Created!;

        Assert.Equal(typeof(EntryLifecycleSpot), descriptor.HandlerType);
        Assert.Equal(typeof(MemberLifecycleActor), descriptor.ActorType);
        Assert.True(descriptor.PassRequestArgument);
    }

    private sealed class MemberLifecycleSpot : IZLinkSpot<MemberLifecycleActor>
    {
        public string? JoinedActorId { get; private set; }
        public IZLinkSpotContext Context => throw new NotSupportedException();

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
        }

        public ValueTask OnJoinedActorAsync(
            MemberLifecycleActor actor,
            CancellationToken cancellationToken)
        {
            _ = cancellationToken;
            JoinedActorId = actor.ActorId;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnLeaveActorAsync(
            MemberLifecycleActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = cancellationToken;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class MemberLifecycleActor(string actorId) : IZLinkActor
    {
        public string ActorId { get; } = actorId;

        public IZLinkActorContext Context { get; } = new TestActorContext(actorId);
    }

    private sealed class EntryLifecycleSpot : IZLinkEntrySpot<MemberLifecycleActor>
    {
        public IZLinkEntrySpotContext Context => throw new NotSupportedException();

        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            MemberLifecycleActor actor,
            ZLinkMessage createRequest,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = createRequest;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
        }

        public ValueTask OnJoinedActorAsync(
            MemberLifecycleActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnLeaveActorAsync(
            MemberLifecycleActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    private sealed record HandlerMessage;

    private sealed record ActorHandlerMessage(string Value);

    private sealed class HandlerLifetime
    {
        public List<object> Invocations { get; } = [];

        public int DisposeCount { get; set; }
    }

    private sealed class ActorHandlerLifetime
    {
        public List<(ActorPacketHandler Handler, ActorScopedDependency Dependency)>
            Invocations { get; } = [];
    }

    private sealed class ActorScopedDependency;

    private sealed class ActorPacketHandler(
        ActorScopedDependency dependency,
        ActorHandlerLifetime lifetime)
        : IZLinkSpotActorSendHandler<
            MemberLifecycleSpot,
            MemberLifecycleActor,
            ActorHandlerMessage>,
          IAsyncDisposable
    {
        public int DisposeCount { get; private set; }

        public ValueTask HandleAsync(
            MemberLifecycleSpot spot,
            MemberLifecycleActor actor,
            IZLinkMessageContext context,
            ActorHandlerMessage message,
            CancellationToken cancellationToken)
        {
            _ = spot;
            _ = actor;
            _ = context;
            _ = message;
            cancellationToken.ThrowIfCancellationRequested();
            lifetime.Invocations.Add((this, dependency));
            return ValueTask.CompletedTask;
        }

        public ValueTask DisposeAsync()
        {
            DisposeCount++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class DisposablePacketHandler(HandlerLifetime lifetime)
        : IZLinkSpotPacketHandler<MemberLifecycleSpot, HandlerMessage>, IDisposable
    {
        public ValueTask HandleAsync(
            MemberLifecycleSpot spot,
            HandlerMessage message,
            CancellationToken cancellationToken)
        {
            _ = spot;
            _ = message;
            cancellationToken.ThrowIfCancellationRequested();
            lifetime.Invocations.Add(this);
            return ValueTask.CompletedTask;
        }

        public void Dispose()
        {
            lifetime.DisposeCount++;
        }
    }

    private sealed class FailingHandler
    {
        public FailingHandler()
        {
            throw new InvalidOperationException("handler initialization failed");
        }
    }
}
