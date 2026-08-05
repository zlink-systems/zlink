using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task Flowless_Actor_Stream_Ingress_Creates_One_Inbound_Flow_For_Join_And_Reply()
    {
        var node = new CapturingSpotNode
        {
            ActorLookupResult = new ZLinkBackendActorRef(
                RoutingId.From("entry-node"),
                "flow-actor",
                1)
        };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal,
            includeJoinTarget: true);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<JoinTargetSpot>();
            var probe = runtime.Services.GetRequiredService<FlowJoinProbe>();
            probe.TargetSpotId = target.Spot.SpotId;

            Assert.Null(ZLinkFlowContext.Current);
            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(
                    actorRef,
                    "flow-join",
                    "join-request",
                    requestId: 91,
                    flags: 1),
                CancellationToken.None);

            Assert.Null(ZLinkFlowContext.Current);
            Assert.Equal("flow-actor", actor.ActorId);
            // Ledger §2.3: deferred Join은 handler terminal 이후에 실행한다.
            // C++·Node·.NET 모두 dispatch 완료와 결합하지 않으므로 관찰 전에 기다린다.
            var joinDeadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
            while (probe.JoinFlow is null && DateTime.UtcNow < joinDeadline)
                await Task.Delay(5);
            var joinFlow = Assert.IsType<ZLinkFlowValue>(probe.JoinFlow);
            Assert.True(ZlinkStreamFlowId.IsValid(joinFlow.FlowId));
            Assert.Equal(ZLinkFlowOrigin.Inbound, joinFlow.Origin);

            var reply = Assert.Single(node.NoBindReplies);
            var decoded = DecodeReplyFrame<ProbeReply>(Assert.Single(reply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Response, decoded.Header.Kind);
            Assert.Equal("join-request:flow-actor", decoded.Payload.Value);
            Assert.Equal(joinFlow.FlowId, decoded.Header.FlowId);
            Assert.Equal(ZlinkStreamFlowOrigin.Inbound, decoded.Header.FlowOrigin);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Spot_Actor_Request_Ingress_Emits_Received_Then_Replied_With_The_Wire_Identity()
    {
        var node = new CapturingSpotNode();
        var observer = new CapturingMessageFlowObserver();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            observer,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            await DispatchEntryActorPartsAsync(
                runtime,
                CreateExactActorRequestParts(actorRef),
                CancellationToken.None);

            for (var attempt = 0;
                 attempt < 100 && CountExactActorRequestEvents(observer, actor.ActorId) < 2;
                 attempt++)
                await Task.Delay(5);

            var events = observer.Events
                .Where(flow => flow.Surface == "actor"
                               && flow.MessageKind == "request"
                               && flow.ActorId == actor.ActorId)
                .ToArray();
            Assert.Equal(2, events.Length);
            Assert.Equal(
                ["received", "replied"],
                events.Select(flow => flow.Phase));
            Assert.All(events, flow =>
            {
                Assert.Equal(ExactActorFlowId, flow.FlowId);
                Assert.Equal("application", flow.FlowOrigin);
                Assert.Equal("corr-1", flow.CorrelationId);
            });
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static int CountExactActorRequestEvents(
        CapturingMessageFlowObserver observer,
        string actorId)
    {
        return observer.Events.Count(flow =>
            flow.Surface == "actor"
            && flow.MessageKind == "request"
            && flow.ActorId == actorId);
    }

    private static IReadOnlyList<ZLinkBackendActorPart> CreateExactActorRequestParts(
        ZLinkBackendActorRef actorRef)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq
            | ZlinkStreamHeaderFlags.HasCorrelationId
            | ZlinkStreamHeaderFlags.HasFlowId,
            new ZlinkStreamRequestSeq(7),
            "request",
            ZlinkStreamMetadata.Empty,
            "corr-1",
            ExactActorFlowId,
            ZlinkStreamFlowOrigin.Application);
        var replyRoute = new ZLinkBackendActorRouteContext(
            default,
            0,
            1,
            1,
            1,
            77,
            1,
            "exact-flow-reply-capability");
        return
        [
            new ZLinkBackendActorPart(
                actorRef,
                RoutingId.From("source-node"),
                RoutingId.From("source-session"),
                77,
                1,
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
                true,
                RouteContext: replyRoute),
            new ZLinkBackendActorPart(
                actorRef,
                RoutingId.From("source-node"),
                RoutingId.From("source-session"),
                77,
                1,
                Message.From(ZLinkEnvelopeCodec.EncodeJsonBytes("exact", typeof(string))),
                false,
                RouteContext: replyRoute)
        ];
    }

    private const string ExactActorFlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2e";
}
