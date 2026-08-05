using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Workflow.Support;

internal sealed class StaleSpotIdProbe
{
    private readonly object _gate = new();
    private string? _spotId;

    public void Capture(string spotId)
    {
        lock (_gate) _spotId = spotId;
    }

    public async ValueTask<StaleSpotIdProbeRes> ExecuteAsync(
        IZLinkSpotClient routes,
        CancellationToken cancellationToken)
    {
        string spotId;
        lock (_gate)
            spotId = _spotId
                     ?? throw new InvalidOperationException("A Spot ID has not been captured.");
        try
        {
            _ = await routes.RequestToSpot(spotId, new ReadWorkflowReq())
                .Async<ReadWorkflowRes>(cancellationToken);
            return new StaleSpotIdProbeRes(false, null);
        }
        catch (ZLinkFrameworkException error)
        {
            return new StaleSpotIdProbeRes(true, error.Kind.ToString());
        }
    }
}
