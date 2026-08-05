using ObservabilityOps.Server.Workflow.Spots;
using ObservabilityOps.Server.Workflow.Support;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Workflow.Handlers;

internal sealed class AdvanceWorkflowHandler
    : IZLinkSpotRequestHandler<WorkflowSpot, AdvanceWorkflowReq, AdvanceWorkflowRes>
{
    public ValueTask<AdvanceWorkflowRes> HandleAsync(WorkflowSpot spot, AdvanceWorkflowReq request,
        CancellationToken cancellationToken) => spot.AdvanceAsync(request, cancellationToken);
}

internal sealed class ReadWorkflowHandler
    : IZLinkSpotRequestHandler<WorkflowSpot, ReadWorkflowReq, ReadWorkflowRes>
{
    public ValueTask<ReadWorkflowRes> HandleAsync(WorkflowSpot spot, ReadWorkflowReq request,
        CancellationToken cancellationToken)
    {
        _ = request;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new ReadWorkflowRes(
            spot.Context.SpotId.ToString(), spot.Context.NodeRid.ToString(), spot.Version, spot.State));
    }
}

internal sealed class WorkflowSignalHandler(WorkflowEvidenceStore evidence)
    : IZLinkSpotPacketHandler<WorkflowSpot, WorkflowSignalReq>
{
    public ValueTask HandleAsync(WorkflowSpot spot, WorkflowSignalReq message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"workflow-signal|rid={spot.Context.SpotId}|marker={message.Marker}"
                     + $"|node={spot.Context.NodeRid}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class DiagnosticsBeforeHandler
    : IZLinkSpotRequestHandler<
        WorkflowSpot,
        DiagnosticsBeforeReq,
        DiagnosticsProbeRes>
{
    public ValueTask<DiagnosticsProbeRes> HandleAsync(
        WorkflowSpot spot,
        DiagnosticsBeforeReq request,
        CancellationToken cancellationToken) =>
        Complete(spot, request.Marker, cancellationToken);

    internal static ValueTask<DiagnosticsProbeRes> Complete(
        WorkflowSpot spot,
        string marker,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new DiagnosticsProbeRes(
            marker, spot.Context.NodeRid.ToString()));
    }
}

internal sealed class DiagnosticsOffHandler
    : IZLinkSpotRequestHandler<
        WorkflowSpot,
        DiagnosticsOffReq,
        DiagnosticsProbeRes>
{
    public ValueTask<DiagnosticsProbeRes> HandleAsync(
        WorkflowSpot spot,
        DiagnosticsOffReq request,
        CancellationToken cancellationToken) =>
        DiagnosticsBeforeHandler.Complete(
            spot, request.Marker, cancellationToken);
}

internal sealed class DiagnosticsErrorsHandler
    : IZLinkSpotRequestHandler<
        WorkflowSpot,
        DiagnosticsErrorsReq,
        DiagnosticsProbeRes>
{
    public ValueTask<DiagnosticsProbeRes> HandleAsync(
        WorkflowSpot spot,
        DiagnosticsErrorsReq request,
        CancellationToken cancellationToken) =>
        DiagnosticsBeforeHandler.Complete(
            spot, request.Marker, cancellationToken);
}

internal sealed class DiagnosticsAfterHandler
    : IZLinkSpotRequestHandler<
        WorkflowSpot,
        DiagnosticsAfterReq,
        DiagnosticsProbeRes>
{
    public ValueTask<DiagnosticsProbeRes> HandleAsync(
        WorkflowSpot spot,
        DiagnosticsAfterReq request,
        CancellationToken cancellationToken) =>
        DiagnosticsBeforeHandler.Complete(
            spot, request.Marker, cancellationToken);
}

internal sealed class PublishProjectionHandler
    : IZLinkSpotRequestHandler<WorkflowSpot, PublishProjectionReq, PublishProjectionRes>
{
    public ValueTask<PublishProjectionRes> HandleAsync(WorkflowSpot spot, PublishProjectionReq request,
        CancellationToken cancellationToken) => spot.PublishAsync(request, cancellationToken);
}

internal sealed class ProjectionUpdatedHandler(WorkflowEvidenceStore evidence)
    : IZLinkSpotSubscriptionHandler<ProjectionSpot, ProjectionUpdatedEvent>
{
    public ValueTask HandleAsync(ProjectionSpot spot, ProjectionUpdatedEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"projection-received|subscriber={spot.Context.SpotId}|rid={message.WorkflowRid}"
                     + $"|version={message.Version}|marker={message.Marker}|node={spot.Context.NodeRid}");
        return ValueTask.CompletedTask;
    }
}
