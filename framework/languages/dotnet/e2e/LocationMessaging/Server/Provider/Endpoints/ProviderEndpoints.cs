using LocationMessaging.Server.Provider.Configuration;
using LocationMessaging.Server.Provider.Infrastructure;
using LocationMessaging.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;

namespace LocationMessaging.Server.Provider.Endpoints;

internal static class ProviderEndpoints
{
    public static void MapProviderEndpoints(this WebApplication app, ServerOptions options)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Role, options.Rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        // Public topology query reports the application-visible peer state.
        app.MapGet("/locations/peers", async (
            string? mesh,
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var peers = await query.ListTopologyAsync(
                new ZLinkLocationTopologyFilter(
                    mesh ?? throw new InvalidOperationException("mesh query parameter is required.")),
                cancellationToken: cancellationToken);
            return Results.Ok(peers.Items.Select(ToPeerRow).ToArray());
        });
        app.MapPost("/locations/peers/wait", async (
            PeerLocationWaitReq request,
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var deadline = DateTimeOffset.UtcNow + timeout;
            PeerLocationRow[] rows = [];
            while (DateTimeOffset.UtcNow < deadline)
            {
                rows = (await query.ListTopologyAsync(
                        new ZLinkLocationTopologyFilter(request.MeshName),
                        cancellationToken: cancellationToken))
                    .Items
                    .Select(ToPeerRow)
                    .ToArray();
                var matches = rows.Where(row =>
                        row.Role == request.Role
                        && row.State == nameof(ZLinkLocationTopologyState.Ready)
                        && row.NodeRid is { } nodeRid
                        && (nodeRid == request.NodeRid
                            || nodeRid.StartsWith($"{request.NodeRid}-", StringComparison.Ordinal)))
                    .ToArray();
                var reached = request.Present
                    ? matches.Length == 1
                      && (request.Endpoint is null || matches[0].Endpoint == request.Endpoint)
                    : matches.Length == 0;
                if (reached) return Results.Ok(rows);

                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
            }

            return Results.Problem(
                $"Peer row did not reach the requested state for rid={request.NodeRid}.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        // The second endpoint verifies the same public topology projection.
        app.MapGet("/locations/member-peers", async (
            string? mesh,
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var peers = await query.ListTopologyAsync(
                new ZLinkLocationTopologyFilter(
                    mesh ?? throw new InvalidOperationException("mesh query parameter is required.")),
                cancellationToken: cancellationToken);
            return Results.Ok(peers.Items.Select(ToPeerRow).ToArray());
        });
        app.MapGet("/locations/status", async (
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var status = await query.GetStatusAsync(cancellationToken);
            return Results.Ok(new LocationStatusRes(
                status.StoreHealthy,
                status.OwnerLeaseHealthy));
        });
        app.MapGet("/runtime/status", (IZLinkFrameworkRuntime runtime) =>
        {
            var inbound = runtime.Status.InboundDispatch;
            return Results.Ok(new RuntimeInboundStatusRes(
                inbound.ApplicationHwmBytes,
                inbound.PendingPayloadBytes,
                inbound.QueuedPayloadBytes,
                inbound.ActivePayloadBytes,
                inbound.ApplicationReceivePaused));
        });
        app.MapPost("/profile/backpressure/reset", (BackpressureGate gate) =>
        {
            gate.Reset();
            return Results.Ok(new { status = "ready" });
        });
        app.MapPost("/profile/backpressure/release", (BackpressureGate gate) =>
        {
            gate.Release();
            return Results.Ok(new { status = "released" });
        });
        app.MapPost("/profile/request", async (
            ProfileReq request,
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var reply = await channel.RequestToChannel("profile", request)
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<ProfileRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/profile/command", async (
            ProfileMsg command,
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            await channel.SendToChannel("profile", command)
                .Async(cancellationToken);
            return Results.Ok(new { status = "sent" });
        });
        app.MapPost("/profile/weight", (
            int weight,
            IZLinkRouteMeshRuntimeOptions runtimeOptions) =>
        {
            // The E2E uses the public runtime option to exclude the local
            // member and prove that ChannelName-only calls reach a remote
            // MeshNode. No target RID, endpoint or MeshName enters the call.
            runtimeOptions.Channel("profile").Weight = weight;
            return Results.Ok(new { weight });
        });
        app.MapPost("/profile/route/request", async (
            ScenarioRoutePing request,
            IZLinkRouteClient route,
            CancellationToken cancellationToken) =>
        {
            var reply = await route.RequestToNode("profile.route", RoutingId.From("api-b"), request)
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<ScenarioRoutePong>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/profile/route/missing", async (
            ScenarioRoutePing request,
            IZLinkRouteClient route) =>
        {
            try
            {
                await route.RequestToNode("profile.route", RoutingId.From("missing-rid"), request)
                    .Timeout(TimeSpan.FromMilliseconds(300))
                    .Async<ScenarioRoutePong>();
            }
            catch (ZLinkFrameworkException error) when (
                error.Kind == ZLinkFrameworkErrorKind.NotFound)
            {
                return Results.Ok(new ExpectedFailureRes(error.Kind.ToString()));
            }

            throw new InvalidOperationException(
                "A request to a missing route target completed without NotFound.");
        });
        app.MapPost("/profile/route/target", async (
            TargetedRoutePing request,
            IZLinkRouteClient route,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var reply = await route.RequestToNode(
                        "profile.route",
                        RoutingId.From(request.TargetRid),
                        new ScenarioRoutePing(request.Value))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<ScenarioRoutePong>(cancellationToken);
                return Results.Ok(new ExpectedFailureRes($"UnexpectedReply:{reply.ProviderRid}"));
            }
            catch (ZLinkFrameworkException error) when (
                error.Kind is ZLinkFrameworkErrorKind.NotFound
                    or ZLinkFrameworkErrorKind.Unavailable)
            {
                return Results.Ok(new ExpectedFailureRes(error.Kind.ToString()));
            }
            catch (TimeoutException)
            {
                return Results.Ok(new ExpectedFailureRes("Timeout"));
            }
        });
        app.MapPost("/admin/drain", async (
            IZLinkFrameworkRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var relocation = await runtime.RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30)
                },
                cancellationToken);
            var result = relocation.Outcome == ZLinkFrameworkRelocationOutcome.Relocated
                ? await runtime.ShutdownAsync(TimeSpan.FromSeconds(30), cancellationToken)
                : new ZLinkFrameworkTerminationResult(
                    ZLinkFrameworkTerminationOutcome.ForceStopped,
                    ZLinkFrameworkTerminationReason.TeardownFailed);
            //  A blocked relocation collapses into one reason enum, which does
            //  not say what blocked it; carry the relocation reason through.
            var reason = relocation.Outcome == ZLinkFrameworkRelocationOutcome.Relocated
                ? result.Reason == ZLinkFrameworkTerminationReason.None
                    ? null
                    : result.Reason.ToString()
                : $"{result.Reason}:{relocation.Reason}";
            return Results.Ok(new DrainResultRes(result.Outcome.ToString(), reason));
        });
        app.MapPost("/evidence/clear", (EvidenceStore evidence) =>
        {
            evidence.Clear();
            return Results.Ok(new { status = "cleared" });
        });
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            try
            {
                var snapshot = await evidence.WaitUntilAsync(
                    line => line.Contains(request.Contains, StringComparison.Ordinal),
                    timeout,
                    cancellationToken);
                return Results.Ok(snapshot);
            }
            catch (TimeoutException error)
            {
                return Results.Problem(
                    error.Message,
                    statusCode: StatusCodes.Status504GatewayTimeout);
            }
        });
        app.MapPost("/evidence/wait-count", async (
            EvidenceCountWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 120000));
            try
            {
                var snapshot = await evidence.WaitUntilCountAsync(
                    request.Contains,
                    Math.Max(1, request.MinimumCount),
                    timeout,
                    cancellationToken);
                return Results.Ok(snapshot);
            }
            catch (TimeoutException error)
            {
                return Results.Problem(
                    error.Message,
                    statusCode: StatusCodes.Status504GatewayTimeout);
            }
        });
        app.MapPost("/evidence/wait-quiet", async (
            EvidenceQuietWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var quietPeriod = TimeSpan.FromMilliseconds(Math.Clamp(request.QuietMilliseconds, 1, 5000));
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 60000));
            try
            {
                var snapshot = await evidence.WaitUntilQuietAsync(
                    request.Contains,
                    quietPeriod,
                    timeout,
                    cancellationToken);
                return Results.Ok(snapshot);
            }
            catch (TimeoutException error)
            {
                return Results.Problem(
                    error.Message,
                    statusCode: StatusCodes.Status504GatewayTimeout);
            }
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
    }

    private static PeerLocationRow ToPeerRow(ZLinkLocationTopologyEntry peer)
    {
        return new PeerLocationRow(
            peer.MeshName,
            peer.NodeRid.ToString(),
            "Router",
            peer.Endpoint,
            peer.Draining,
            peer.State.ToString());
    }

}
