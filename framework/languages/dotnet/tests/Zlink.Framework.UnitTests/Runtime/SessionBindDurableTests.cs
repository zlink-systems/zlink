using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
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
}
