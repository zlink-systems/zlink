using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace AutomaticTurnDispatch.Server.Delay;

internal sealed class DelayHandler(NodeOptions node, EvidenceStore evidence)
    : IZLinkRequestHandler<DelayReq, DelayRes>
{
    public async ValueTask<DelayRes> HandleAsync(
        DelayReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        evidence.Add($"delay-started|rid={node.Rid}|request={request.RequestId}|marker={request.Marker}");
        await Task.Delay(TimeSpan.FromMilliseconds(request.DelayMs), cancellationToken);
        evidence.Add($"delay-completed|rid={node.Rid}|request={request.RequestId}|marker={request.Marker}");
        return new DelayRes(request.RequestId, request.Marker, node.Rid);
    }
}
