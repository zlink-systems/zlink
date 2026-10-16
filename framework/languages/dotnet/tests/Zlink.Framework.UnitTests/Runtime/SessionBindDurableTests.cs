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
        node.NodeRequestHandler = (parts, timeout) =>
        {
            attempts++;
            Assert.True(node.LastNodeRequestDurable);
            Assert.Equal(1, attempts);
            Assert.True(timeout > TimeSpan.Zero);
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts[0]);
            var responseHeader = header with { Kind = ZLinkMessageKind.Response };
            var response = new ZLinkRemoteSessionBindResponse(
                !rejected,
                1,
                "entry",
                RoutingId.From("remote-node").ToBytes().ToArray(),
                3,
                5,
                7
            );
            return ValueTask.FromResult(
                new ZLinkBackendRouteReceived(
                    ZLinkClientCallCodec.EncodeEnvelopeParts(
                        responseHeader,
                        response,
                        new ZLinkFrameworkRegistration().Codecs
                    ),
                    null,
                    null,
                    null,
                    null
                )
            );
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
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
                new ActorRef("remote-bind-actor", 1, "entry", RoutingId.From("remote-node")),
                CancellationToken.None
            );
            if (rejected)
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => bind.AsTask());
                Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
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
