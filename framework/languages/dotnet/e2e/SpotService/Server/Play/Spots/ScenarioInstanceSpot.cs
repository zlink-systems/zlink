using SpotService.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.Play.Spots;

internal sealed class ScenarioInstanceSpot(
    IZLinkInstanceSpotContext context,
    EvidenceStore evidence) : IZLinkInstanceSpot
{
    public IZLinkInstanceSpotContext Context { get; } = context;

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"instance-initialize|rid={evidence.Rid}|spot={Context.SpotId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        cleanupCancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"instance-closing|rid={evidence.Rid}|spot={Context.SpotId}"
            + $"|reason={context.Reason}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class ScenarioInstanceStateHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioInstanceSpot, StateReq, StateRes>
{
    public ValueTask<StateRes> HandleAsync(
        ScenarioInstanceSpot spot,
        StateReq request,
        CancellationToken cancellationToken)
    {
        _ = request;
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"instance-request|rid={evidence.Rid}|spot={spot.Context.SpotId}");
        return ValueTask.FromResult(new StateRes(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            0));
    }
}

[ZLinkSpotRequestHandler("InstanceColdRequest")]
internal sealed class ScenarioInstanceColdRequestHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioInstanceSpot, InstanceColdRequest, InstanceColdRequestReply>
{
    public ValueTask<InstanceColdRequestReply> HandleAsync(
        ScenarioInstanceSpot spot,
        InstanceColdRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"instance-request|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|operation={request.OperationId}");
        return ValueTask.FromResult(new InstanceColdRequestReply(
            spot.Context.SpotId,
            request.OperationId,
            spot.Context.NodeRid.ToString()));
    }
}

[ZLinkSpotPacketHandler("InstanceColdSend")]
internal sealed class ScenarioInstanceColdSendHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<ScenarioInstanceSpot, InstanceColdSend>
{
    public ValueTask HandleAsync(
        ScenarioInstanceSpot spot,
        InstanceColdSend message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"instance-send|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|operation={message.OperationId}");
        return ValueTask.CompletedTask;
    }
}

internal sealed record InstanceColdRequestReply(
    string SpotId,
    string OperationId,
    string NodeRid);
