using SpotService.Shared;
using SpotService.Server.Play.Spots;
using Systems.Zlink;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.Play.Endpoints;

internal static class InstanceSpotEndpoints
{
    public static void MapInstanceSpotEndpoints(WebApplication app)
    {
        app.MapPost("/instance/location", async (
            IZLinkLocationRuntimeQuery locations,
            InstanceLocationReq request,
            CancellationToken cancellationToken) =>
        {
            var entry = await locations.FindSpotLocationAsync(
                request.SpotId,
                cancellationToken);
            return Results.Ok(entry is null
                ? new InstanceLocationRes(false, "Missing", string.Empty, 0)
                : new InstanceLocationRes(
                    true,
                    entry.State.ToString(),
                    entry.NodeRid.ToString(),
                    entry.ObjectGeneration));
        });

        app.MapPost("/instance/cold-request", async (
            IZLinkSpotClient spots,
            InstanceColdProbeReq request,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var reply = await spots
                    .RequestToSpot(
                        request.SpotId,
                        new InstanceColdReq(request.OperationId))
                    .InstanceSpot(SpotServiceNames.InstanceSpotType)
                    .Timeout(TimeSpan.FromSeconds(10))
                    .Async<InstanceColdRes>(cancellationToken);
                return Results.Ok(new InstanceColdProbeRes(
                    request.SpotId,
                    request.OperationId,
                    Succeeded: true,
                    ErrorKind: string.Empty,
                    reply.NodeRid,
                    Value: 0));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new InstanceColdProbeRes(
                    request.SpotId,
                    request.OperationId,
                    Succeeded: false,
                    error.Kind.ToString(),
                    NodeRid: string.Empty,
                    Value: 0));
            }
        });

        app.MapPost("/instance/cold-send", async (
            IZLinkSpotClient spots,
            InstanceColdSendReq request,
            CancellationToken cancellationToken) =>
        {
            try
            {
                await spots
                    .SendToSpot(
                        request.SpotId,
                        new InstanceColdMsg(request.OperationId))
                    .InstanceSpot(SpotServiceNames.InstanceSpotType)
                    .Async(cancellationToken);
                return Results.Ok(new InstanceColdSendRes(
                    request.SpotId,
                    request.OperationId,
                    Accepted: true,
                    ErrorKind: string.Empty));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new InstanceColdSendRes(
                    request.SpotId,
                    request.OperationId,
                    Accepted: false,
                    error.Kind.ToString()));
            }
        });
    }
}
