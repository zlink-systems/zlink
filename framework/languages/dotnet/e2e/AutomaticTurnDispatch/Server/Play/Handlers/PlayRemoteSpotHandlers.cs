using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace AutomaticTurnDispatch.Server.Play.Handlers;

[ZLinkSpotRequestHandler("RemoteSpotAwaitReq")]
internal sealed class RemoteSpotAwaitHandler(
    EvidenceStore evidence)
    : IZLinkSpotRequestHandler<AwaitProbeSpot, RemoteSpotAwaitReq, AutomaticTurnDispatchRes>
{
    public async ValueTask<AutomaticTurnDispatchRes> HandleAsync(
        AwaitProbeSpot spot,
        RemoteSpotAwaitReq request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"remote-await-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|handler=spot");
        var call = spot.Context.Outbound.RequestToSpot(
                request.TargetSpotRid,
                new AwaitReq(request.RequestId, request.DelayMs, "remote-spot"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"remote-await-released|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|handler=spot");
        var targetReply = await call.Async<AutomaticTurnDispatchRes>(cancellationToken);
        evidence.Add(
            $"remote-await-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|targetNode={targetReply.NodeRid}|handler=spot");
        evidence.Add(
            $"remote-await-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|targetNode={targetReply.NodeRid}|handler=spot");
        return AwaitReplies.Reply("probe-D2", request.RequestId, spot, "remote-await-completed");
    }
}

[ZLinkSpotPacketHandler("RemoteSpotAwaitMsg")]
internal sealed class RemoteSpotAwaitCommandHandler(
    EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, RemoteSpotAwaitMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        RemoteSpotAwaitMsg request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"remote-await-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|handler=spot");
        var call = spot.Context.Outbound.RequestToSpot(
                request.TargetSpotRid,
                new AwaitReq(request.RequestId, request.DelayMs, "remote-spot"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"remote-await-released|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|handler=spot");
        var targetReply = await call.Async<AutomaticTurnDispatchRes>(cancellationToken);
        evidence.Add(
            $"remote-await-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|targetNode={targetReply.NodeRid}|handler=spot");
        evidence.Add(
            $"remote-await-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|target={request.TargetSpotRid}|targetNode={targetReply.NodeRid}|handler=spot");
    }
}
