using LocationMessaging.Server.Workflow.Infrastructure;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace LocationMessaging.Server.Workflow.Handlers;

internal sealed class WorkflowRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<WorkflowReq, WorkflowRes>
{
    public ValueTask<WorkflowRes> HandleAsync(
        WorkflowReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"workflow-request|rid={evidence.Rid}|value={request.Value}|packet={context.PacketName}");
        return ValueTask.FromResult(new WorkflowRes($"workflow:{request.Value}", evidence.Rid));
    }
}
