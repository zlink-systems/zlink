using ObservabilityOps.Server.Workflow.Handlers;
using ObservabilityOps.Server.Workflow.Support;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Workflow.Spots;

internal sealed class ProjectionSpot(
    IZLinkSpotContext context,
    WorkflowEvidenceStore evidence) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddSubscribe<ProjectionUpdatedHandler>(
            ObservabilityNames.WorkflowMesh,
            "observability.projection"); // This Spot receives projection fanout.

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _ = request.Decode<CreateWorkflowReq>();
        evidence.Add($"workflow-created|rid={Context.SpotId}|kind=subscriber|version=0|node={Context.NodeRid}");
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }
}
