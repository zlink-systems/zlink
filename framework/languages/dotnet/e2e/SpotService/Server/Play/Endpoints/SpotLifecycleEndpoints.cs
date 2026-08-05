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
        app.MapPost("/spot/a9/start", (
            SpotInitializationCoordinator coordinator,
            GatedSpotCreateReq request) =>
        {
            coordinator.Start(request.SpotRid);
            return Results.Ok(new GatedSpotCreateRes(
                request.SpotRid,
                string.Empty,
                "Started"));
        });
        app.MapPost("/spot/a9/status", async (
            SpotInitializationCoordinator coordinator,
            GatedSpotCreateReq request) =>
        {
            var result = await coordinator.CompleteAsync(request.SpotRid);
            return Results.Ok(new GatedSpotCreateRes(
                result.Spot.SpotId,
                result.Spot.NodeRid.ToString(),
                result.State.ToString()));
        });
        app.MapPost("/spot/a9/probe", async (
            IZLinkSpotManager spots,
            IZLinkSpotClient spotsClient,
            GatedSpotCreateReq request,
            CancellationToken cancellationToken) =>
        {
            var found = await spots.FindAsync(request.SpotRid, cancellationToken);
            var requestSucceeded = false;
            var requestErrorKind = string.Empty;
            var requestNodeRid = string.Empty;
            var value = 0;
            try
            {
                var reply = await spotsClient
                    .RequestToSpot(request.SpotRid, new StateReq("add", 1))
                    .Timeout(TimeSpan.FromMilliseconds(1000))
                    .Async<StateRes>(cancellationToken);
                requestSucceeded = true;
                requestNodeRid = reply.NodeRid;
                value = reply.Value;
            }
            catch (ZLinkFrameworkException error)
            {
                requestErrorKind = error.Kind.ToString();
            }
            catch (TimeoutException)
            {
                requestErrorKind = "Timeout";
            }
            catch (Exception error)
            {
                requestErrorKind = error.GetType().Name;
            }

            return Results.Ok(new SpotPublicationProbeRes(
                request.SpotRid,
                found is not null,
                found?.NodeRid.ToString() ?? string.Empty,
                requestSucceeded,
                requestErrorKind,
                requestNodeRid,
                value));
        });
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
        app.MapPost("/spot/create-automatic-batch", async (
            IZLinkSpotManager spots,
            IZLinkSpotClient spotClient,
            EvidenceStore evidence,
            NodeOptions node,
            AutomaticSpotBatchReq request,
            CancellationToken cancellationToken) =>
        {
            var count = Math.Clamp(request.Count, 1, 256);
            var created = await Task.WhenAll(Enumerable.Range(0, count).Select(_ =>
                spots.Create(SpotServiceNames.UserSpotType).Async(cancellationToken).AsTask()));
            if (created.Any(result => result.State != ZLinkSpotCreateState.Created))
                throw new InvalidOperationException("Automatic Spot creation did not produce only Created results.");

            var spotIds = created.Select(result => result.Spot.SpotId).ToArray();
            var stateResults = await Task.WhenAll(spotIds.Select(spotId =>
                RequestSpotStateAsync(
                    spotClient,
                    spotId,
                    new StateReq("add", 1))));
            var distinctIds = spotIds.Distinct(StringComparer.Ordinal).Count();
            var successfulRequests = stateResults.Count(result => result.Value == 1);
            evidence.Add(
                $"automatic-spot-batch|rid={node.Rid}|requested={count}"
                + $"|created={created.Length}|distinct={distinctIds}"
                + $"|requests={successfulRequests}");
            return Results.Ok(new AutomaticSpotBatchRes(
                count,
                created.Length,
                distinctIds,
                successfulRequests,
                spotIds));
        });
        app.MapPost("/spot/id-boundary", async (
            IZLinkSpotManager spots,
            IZLinkSpotClient spotClient,
            EvidenceStore evidence,
            NodeOptions node,
            CancellationToken cancellationToken) =>
        {
            var validIds = new[]
            {
                "x",
                new string('b', 255),
                "Room",
                "room",
                "\u00e9",
                "e\u0301"
            };
            var created = await Task.WhenAll(validIds.Select(spotId =>
                spots.GetOrCreate(spotId, SpotServiceNames.UserSpotType)
                    .Async(cancellationToken)
                    .AsTask()));
            var found = await Task.WhenAll(validIds.Select(spotId =>
                spots.FindAsync(spotId, cancellationToken).AsTask()));
            var stateResults = await Task.WhenAll(validIds.Select(spotId =>
                RequestSpotStateAsync(
                    spotClient,
                    spotId,
                    new StateReq("add", 1))));

            var invalidId = new string('c', 256);
            var invalidFactoryMarker =
                $"spot-created|rid={node.Rid}|spot={invalidId}";
            var before = evidence.Snapshot();
            var invalidErrorKind = await CaptureErrorKindAsync(() =>
                spots.GetOrCreate(invalidId, SpotServiceNames.UserSpotType)
                    .Async(cancellationToken)
                    .AsTask());
            var after = evidence.Snapshot();
            var invalidFactoryCalls = after.Count(line =>
                    line.Contains(invalidFactoryMarker, StringComparison.Ordinal))
                - before.Count(line =>
                    line.Contains(invalidFactoryMarker, StringComparison.Ordinal));
            var foundIds = found.Select(value => value?.SpotId ?? string.Empty).ToArray();
            var exactEquality = validIds.SequenceEqual(foundIds, StringComparer.Ordinal)
                && created.Select(value => value.Spot.SpotId)
                    .SequenceEqual(validIds, StringComparer.Ordinal)
                && stateResults.All(value => value.Value == 1);
            evidence.Add(
                $"spot-id-boundary|rid={node.Rid}|valid={validIds.Length}"
                + $"|invalid={invalidErrorKind}|factory={invalidFactoryCalls}");
            return Results.Ok(new SpotIdBoundaryRes(
                validIds,
                foundIds,
                stateResults.Select(value => value.Value).ToArray(),
                invalidErrorKind,
                invalidFactoryCalls,
                exactEquality));

            static async Task<string> CaptureErrorKindAsync(Func<Task> operation)
            {
                try
                {
                    await operation();
                    return string.Empty;
                }
                catch (ZLinkFrameworkException error)
                {
                    return error.Kind.ToString();
                }
                catch (Exception error)
                {
                    return error.GetType().Name;
                }
            }
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
