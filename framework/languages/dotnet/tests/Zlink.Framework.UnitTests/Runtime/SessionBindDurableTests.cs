using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
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
