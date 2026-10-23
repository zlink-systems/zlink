using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task Logical_Disconnect_Submits_Actor_Unbound_Exactly_Once()
    {
        var flowPath = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        );
        using var flow = flowPath is null ? null : new TestHostMessageFlowListener(flowPath);
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            messageFlowMode: flowPath is null ? null : ZLinkDiagnosticsLevel.Normal
        );
        var closeCount = 0;
        try
        {
            _ = RegisterProbeActor(runtime, actor);
            var stream = new RetainedOutboundCapturingStream(RoutingId.From("logical-disconnect"));
            var context = new ZLinkSessionContext(
                runtime,
                stream,
                new RelaySessionHandlerRegistry(),
                () =>
                {
                    closeCount++;
                    return ValueTask.CompletedTask;
                },
                static _ => ValueTask.CompletedTask
            );
            const string token = "logical-disconnect-binding";
            runtime.BindActorSession(
                actor.ActorId,
                null,
                stream.RoutingId!.Value,
                token,
                objectGeneration: actor.Generation,
                authorityOwnerGeneration: 1,
                meshName: "entry",
                ownerLeaseGeneration: 1
            );
            var registry = new ZLinkSessionActorBindingRegistry(runtime);
            var bound = await registry.BindAsync(
                context,
                new ActorRef(actor.ActorId, actor.Generation, "entry", actor.NodeRid),
                token,
                1,
                1,
                "entry",
                1,
                1,
                1,
                actor.NodeRid,
                "session-owner",
                1,
                CancellationToken.None
            );

            await bound.NotifyDisconnectedAsync();

            Assert.Empty(registry.BoundActors);
            Assert.Equal(0, closeCount);
            Assert.Equal(2, stream.Writes.Count);
            Assert.Equal(
                Systems
                    .Zlink
                    .Stream
                    .Connector
                    .Runtime
                    .Protocol
                    .ZlinkStreamControlProtocol
                    .ActorUnboundPayloadSize,
                stream.Writes[1].Length
            );
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SessionBind_SelectsDurableOwnerAndConsumesOneTerminal(bool rejected)
    {
        var node = new CapturingSpotNode();
        var attempts = 0;
        node.CanonicalBindHandler = (request, timeout, cancellationToken) =>
        {
            attempts++;
            Assert.Equal(1, attempts);
            Assert.True(timeout > TimeSpan.Zero);
            var frames =
                Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.EncodeBoundSessionBind38(
                    request,
                    Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.DecodeContext.Empty
                );
            Assert.Equal(38, frames[0][3]);
            var decoded =
                Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.DecodeBoundSessionBind38(
                    frames,
                    Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.DecodeContext.Empty
                );
            Assert.Equal("actor-a", decoded.Actor.Actor.ActorId.Value);
            return rejected
                ? ValueTask.FromException<Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.RequestSpecificTailCase2?>(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "Bind refused."
                    )
                )
                : ValueTask.FromResult<Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.RequestSpecificTailCase2?>(
                    new(new(1), request.Actor.ExpectedAuthorityOwnerGeneration)
                );
        };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            var context = new ZLinkSessionContext(
                runtime,
                new RetainedOutboundCapturingStream(RoutingId.From("durable-session")),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            var bind = context.ActorCoordinator.BindActorAsync(
                context,
                new ActorRef(actor.ActorId, actor.Generation, "entry", actor.NodeRid),
                CancellationToken.None
            );
            if (rejected)
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => bind.AsTask());
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            }
            else
                Assert.NotNull(await bind);
            Assert.Equal(1, attempts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    // Session-Actor binding §5: bind routes by the caller's ActorRef. The command 38
    // fence (target node generation, authority) must describe that same ActorRef, not a
    // route row cached for the Actor's previous owner (#2507).
    [Fact]
    public async Task SessionBind_FenceDescribesTheSubmittedActorRef_NotAStaleCachedRoute()
    {
        var node = new CapturingSpotNode();
        Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.BoundSessionBind38? sent = null;
        node.CanonicalBindHandler = (request, _, _) =>
        {
            sent = request;
            return ValueTask.FromException<Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.RequestSpecificTailCase2?>(
                new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, "Captured.")
            );
        };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node, preseedActorOwnership: false);
        try
        {
            var rows = runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
            var key = new ZLinkActorLocationKey(actor.ActorId);
            var previous = await rows.ResolveActorRowAsync(key);
            Assert.NotNull(previous);
            Assert.True(rows.HasCachedActorRoute(key));

            var store = new ZLinkProviderLocationRepository(
                runtime.Registration.Locations.StoreInstance!
            );
            var newOwner = RoutingId.From("relocated-owner");
            const ulong newOwnerNodeGeneration = 7;
            var current = await AuthorityLocationTestFixture.PublishActorAsync(
                store,
                previous with
                {
                    ActorRef = previous.ActorRef with { NodeRid = newOwner },
                    OwnerNodeRid = newOwner,
                    OwnerNodeGeneration = newOwnerNodeGeneration,
                },
                replace: true
            );
            Assert.NotNull(current);
            // The resolver cache still describes the previous owner.
            Assert.Equal(actor.NodeRid, (await rows.ResolveActorRowAsync(key))!.ActorRef.NodeRid);

            var submitted = new ActorRef(
                actor.ActorId,
                current.ObjectGeneration,
                previous.ActorRef.MeshName,
                newOwner
            );
            var context = new ZLinkSessionContext(
                runtime,
                new RetainedOutboundCapturingStream(RoutingId.From("relocated-session")),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await context.ActorCoordinator.BindActorAsync(
                    context,
                    submitted,
                    CancellationToken.None
                )
            );

            Assert.NotNull(sent);
            var fence = sent.Value.Actor;
            Assert.Equal(newOwner, RoutingId.From(fence.TargetNodeRid.Value));
            Assert.Equal(current.ObjectGeneration, fence.Actor.ObjectGeneration.Value);
            Assert.Equal(newOwnerNodeGeneration, fence.TargetNodeGeneration.Value);
            Assert.Equal(
                current.AuthorityOwnerGeneration,
                fence.ExpectedAuthorityOwnerGeneration.Value
            );
            Assert.Equal(
                checked((ulong)current.OwnerLeaseGeneration),
                fence.ExpectedOwnerLeaseGeneration.Value
            );
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    // Session-Actor binding §5: the fence always comes from the Store's current row. An Actor
    // that relocates away and back keeps its ActorRef while its authority generation advances,
    // so a cached row of the same ActorRef still carries the old authority (#2507).
    [Fact]
    public async Task SessionBind_FenceCarriesCurrentAuthority_AfterReturningToTheSameNode()
    {
        var node = new CapturingSpotNode();
        Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.BoundSessionBind38? sent = null;
        node.CanonicalBindHandler = (request, _, _) =>
        {
            sent = request;
            return ValueTask.FromException<Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.RequestSpecificTailCase2?>(
                new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, "Captured.")
            );
        };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node, preseedActorOwnership: false);
        try
        {
            var rows = runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
            var key = new ZLinkActorLocationKey(actor.ActorId);
            var cachedRow = await rows.ResolveActorRowAsync(key);
            Assert.NotNull(cachedRow);

            // The Actor leaves and returns: the Store row names the same ActorRef with a
            // newer authority generation, while the route cache still holds the old row.
            var store = new ZLinkProviderLocationRepository(
                runtime.Registration.Locations.StoreInstance!
            );
            var authorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
            var before = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await store.ReadAuthorityAsync(authorityKey)
                )
                .Snapshot;
            var current = Assert
                .IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                    await store.CompareExchangeAuthorityAsync(
                        authorityKey,
                        before.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            before.Payload,
                            ZLinkAuthorityGenerationTransition.NewOwner,
                            new ZLinkLocationOwnerToken(
                                before.OwnerId,
                                before.OwnerLeaseGeneration
                            ),
                            before.Allocation,
                            before.AuthorityOwnerGeneration + 2
                        )
                    )
                )
                .Snapshot;
            Assert.Equal(before.ObjectGeneration, current.ObjectGeneration);
            Assert.NotEqual(cachedRow.AuthorityOwnerGeneration, current.AuthorityOwnerGeneration);
            Assert.Equal(
                cachedRow.AuthorityOwnerGeneration,
                (await rows.ResolveActorRowAsync(key))!.AuthorityOwnerGeneration
            );

            var context = new ZLinkSessionContext(
                runtime,
                new RetainedOutboundCapturingStream(RoutingId.From("returned-session")),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await context.ActorCoordinator.BindActorAsync(
                    context,
                    cachedRow.ActorRef,
                    CancellationToken.None
                )
            );

            Assert.NotNull(sent);
            Assert.Equal(
                current.AuthorityOwnerGeneration,
                sent.Value.Actor.ExpectedAuthorityOwnerGeneration.Value
            );
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }
}
