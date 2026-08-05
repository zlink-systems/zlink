using SpotService.Server.Play.Spots;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Spots;

using Zlink.Framework.Contracts.Locations;

namespace SpotService.Server.Play.Endpoints;

using static PlayHostFactory;

internal static class SpotLifecycleEndpoints
{
    public static void MapSpotLifecycleEndpoints(WebApplication app)
    {
        app.MapPost("/spot/create", async (
            IZLinkSpotManager spots,
            EvidenceStore evidence,
            NodeOptions node,
            CreateSpotReq request) =>
        {
            var createdSpot = await spots
                .GetOrCreate(request.SpotRid, SpotServiceNames.UserSpotType)
                .Async();
            evidence.Add($"create-spot|rid={node.Rid}|spot={createdSpot.Spot.SpotId}|state={createdSpot.State}");
            return Results.Ok(new CreateSpotRes(
                createdSpot.Spot.SpotId,
                node.Rid,
                createdSpot.State.ToString()));
        });
        app.MapPost("/spot/create-alternate", async (
            IZLinkSpotManager spots,
            EvidenceStore evidence,
            NodeOptions node,
            CreateSpotReq request) =>
        {
            var createdSpot = await spots
                .GetOrCreate(request.SpotRid, SpotServiceNames.AlternateSpotType)
                .Async();
            evidence.Add(
                $"create-unsubscribed-spot|rid={node.Rid}|spot={createdSpot.Spot.SpotId}|state={createdSpot.State}");
            return Results.Ok(new CreateSpotRes(
                createdSpot.Spot.SpotId,
                node.Rid,
                createdSpot.State.ToString()));
        });
        app.MapPost("/spot/type-mismatch", async (
            IZLinkSpotManager spots,
            EvidenceStore evidence,
            NodeOptions node,
            SpotTypeMismatchReq request) =>
        {
            var first = await spots
                .GetOrCreate(request.SpotRid, SpotServiceNames.UserSpotType)
                .Async();
            try
            {
                await spots
                    .GetOrCreate(request.SpotRid, SpotServiceNames.AlternateSpotType)
                    .Async();
            }
            catch (ZLinkFrameworkException ex) when (ex.Kind == ZLinkFrameworkErrorKind.TypeMismatch)
            {
                evidence.Add($"spot-type-mismatch|rid={node.Rid}|spot={request.SpotRid}|kind={ex.Kind}");
                return Results.Ok(new SpotTypeMismatchRes(
                    request.SpotRid,
                    true,
                    ex.Kind.ToString(),
                    first.State.ToString()));
            }

            throw new InvalidOperationException("Expected SpotTypeMismatch for reused spot rid.");
        });
        app.MapPost("/spot/close", async (
            IZLinkSpotManager spots,
            EvidenceStore evidence,
            NodeOptions node,
            CloseSpotReq request) =>
        {
            var spot = await spots.FindAsync(request.SpotRid)
                       ?? throw new InvalidOperationException($"Spot '{request.SpotRid}' was not found.");
            var closed = await spots.CloseAsync(spot);
            evidence.Add($"close-spot|rid={node.Rid}|spot={request.SpotRid}|closed={closed}");
            await WaitUntilAsync(
                () => evidence.Snapshot().Any(line =>
                    line.Contains($"spot-closing|rid={node.Rid}|spot={request.SpotRid}", StringComparison.Ordinal)),
                "Expected spot closing evidence.");
            return Results.Ok(new CloseSpotRes(request.SpotRid, closed));
        });
        app.MapPost("/spot/state/request", async (
            IZLinkSpotClient spotsClient,
            SpotStateRouteReq request) =>
        {
            var result = await RequestSpotStateAsync(
                spotsClient,
                request.SpotRid,
                new StateReq(request.Operation, request.Delta));
            return Results.Ok(result);
        });
        app.MapPost("/spot/state/command", async (
            IZLinkSpotClient spotsClient,
            EvidenceStore evidence,
            SpotStateCommandReq request) =>
        {
            await SendSpotCommandAsync(
                spotsClient,
                request.SpotRid,
                new StateMsg(request.Marker));
            return Results.Ok(new SpotStateCommandRes(
                request.SpotRid,
                request.Marker,
                true,
                evidence.Snapshot()));
        });
    }
}
