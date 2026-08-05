using Microsoft.Extensions.Configuration;

using ObservabilityOps.Server.Support;
using ObservabilityOps.Server.Workflow.Spots;
using ObservabilityOps.Server.Workflow.Support;
using ObservabilityOps.Shared;
using StackExchange.Redis;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace ObservabilityOps.Server.Workflow;

internal static class WorkflowHostFactory
{
    private const string WorkflowSpotType = "observability-workflow";
    private const string ProjectionSpotType = "observability-projection";

    public static WebApplication Create(string[] args)
    {
        var options = WorkflowOptions.Parse(args);
        Directory.CreateDirectory(options.LogDir);
        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console => console.SingleLine = true);
        builder.WebHost.UseUrls(options.HttpUrl);
        builder.Services.AddSingleton(options);
        builder.Services.AddSingleton<IConnectionMultiplexer>(_ =>
            ConnectionMultiplexer.Connect(options.RedisEndpoint));
        builder.Services.AddSingleton<WorkflowStateStore>();
        builder.Services.AddSingleton<WorkflowEvidenceStore>();
        builder.Services.AddSingleton<MetricEvidenceCollector>();
        builder.Services.AddSingleton<RelocationOperation>();
        builder.Services.AddSingleton<ShutdownOperation>();
        builder.Services.AddSingleton<StaleSpotIdProbe>();
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"flow-{options.Rid}.log"),
            options.Rid));
        var locationStore = new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; });
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            framework.AddLocationStore(locationStore);
            var locations = framework.ConfigureLocations();
            locations.OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(options.LocationHeartbeatMs);
            locations.OwnerLeaseTtl = TimeSpan.FromMilliseconds(options.LocationLeaseTtlMs);
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            framework.AddHandlersFromAssemblyOf(typeof(WorkflowHostFactory));
            var mesh16 = framework.AddRouteMesh(ObservabilityNames.WorkflowMesh)
                .Listen(options.RouterEndpoint)
                .SetPlacementWeight(100)
                .SetActorLimit(64)
                .SetSpotLimit(64)
                .SetActivationConcurrency(16);
            mesh16.Objects().Server()
                .AddSpotFactory<WorkflowSpot>(
                    WorkflowSpotType,
                    factory => factory
                        .StableTypeLimit(64)
                        .DisableRelocation())
                .AddSpotFactory<ProjectionSpot>(
                    ProjectionSpotType,
                    factory => factory
                        .StableTypeLimit(64)
                        .DisableRelocation());
            // Logical Multicast picks remote nodes that participate in the
            // ChannelName with a positive weight (spec 12 §2), and weight lives
            // on the Server membership. A Client-only node is never a remote
            // delivery candidate, so projection fanout would reach only the
            // subscribers co-located with the publisher.
            mesh16.Channel(ObservabilityNames.WorkflowMesh).Server().SetWeight(100);
        });

        var app = builder.Build();
        _ = app.Services.GetRequiredService<MetricEvidenceCollector>();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Rid }));
        app.MapGet("/identity", async (
            IZLinkLocationRuntimeQuery locations,
            CancellationToken cancellationToken) =>
        {
            var topology = await locations.ListTopologyAsync(
                new ZLinkLocationTopologyFilter(
                    ObservabilityNames.WorkflowMesh),
                cancellationToken: cancellationToken);
            var local = topology.Items.SingleOrDefault(row =>
                string.Equals(
                    row.Endpoint,
                    options.RouterEndpoint,
                    StringComparison.Ordinal));
            return local is null
                ? Results.StatusCode(StatusCodes.Status503ServiceUnavailable)
                : Results.Ok(new NodeIdentityRes(
                    options.Rid,
                    local.NodeRid.ToString()));
        });
        app.MapDiagnosticsControl();
        app.MapPost("/workflows", async (CreateWorkflowReq request, IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var spotType = string.Equals(
                request.Kind, "subscriber", StringComparison.Ordinal)
                ? ProjectionSpotType
                : WorkflowSpotType;
            var created = await spots.GetOrCreate(request.WorkflowRid, spotType)
                .InMesh(ObservabilityNames.WorkflowMesh)
                .Request(request)
                .Async(cancellationToken);
            return Results.Ok(new CreateWorkflowRes(
                created.Spot.SpotId,
                created.Spot.NodeRid.ToString(),
                0,
                "created"));
        });
        app.MapPost("/workflows/{workflowRid}/advance", async (string workflowRid,
            AdvanceWorkflowReq request, IZLinkSpotClient routes,
            CancellationToken cancellationToken) =>
        {
            var response = await routes.RequestToSpot(workflowRid, request)
                .Async<AdvanceWorkflowRes>(cancellationToken);
            return Results.Ok(response);
        });
        app.MapGet("/workflows/{workflowRid}/state", async (string workflowRid,
            IZLinkSpotClient routes, CancellationToken cancellationToken) =>
        {
            var response = await routes.RequestToSpot(workflowRid, new ReadWorkflowReq())
                .Async<ReadWorkflowRes>(cancellationToken);
            return Results.Ok(response);
        });
        app.MapPost("/workflows/{workflowRid}/signal", async (string workflowRid,
            WorkflowSignalReq request, IZLinkSpotClient routes,
            CancellationToken cancellationToken) =>
        {
            await routes.SendToSpot(workflowRid, request).Async(cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/workflows/{workflowRid}/diagnostics-probe/{phase}",
            async (
                string workflowRid,
                string phase,
                IZLinkSpotClient routes,
                CancellationToken cancellationToken) =>
            {
                var marker = $"diagnostics-{phase}";
                var response = phase switch
                {
                    "before" => await routes
                        .RequestToSpot(
                            workflowRid,
                            new DiagnosticsBeforeReq(marker))
                        .Async<DiagnosticsProbeRes>(cancellationToken),
                    "off" => await routes
                        .RequestToSpot(
                            workflowRid,
                            new DiagnosticsOffReq(marker))
                        .Async<DiagnosticsProbeRes>(cancellationToken),
                    "errors" => await routes
                        .RequestToSpot(
                            workflowRid,
                            new DiagnosticsErrorsReq(marker))
                        .Async<DiagnosticsProbeRes>(cancellationToken),
                    "after" => await routes
                        .RequestToSpot(
                            workflowRid,
                            new DiagnosticsAfterReq(marker))
                        .Async<DiagnosticsProbeRes>(cancellationToken),
                    _ => throw new ArgumentOutOfRangeException(nameof(phase))
                };
                return Results.Ok(response);
            });
        app.MapPost("/workflows/{workflowRid}/stale-spot-id/capture", async (string workflowRid,
            IZLinkSpotManager spots, StaleSpotIdProbe probe,
            CancellationToken cancellationToken) =>
        {
            var spot = await spots.FindAsync(workflowRid, cancellationToken)
                ?? throw new InvalidOperationException($"Workflow '{workflowRid}' was not found.");
            probe.Capture(spot.SpotId);
            return Results.Ok();
        });
        app.MapPost("/stale-spot-id/execute", async (StaleSpotIdProbe probe,
            IZLinkSpotClient routes, CancellationToken cancellationToken) =>
            Results.Ok(await probe.ExecuteAsync(routes, cancellationToken)));
        app.MapPost("/workflows/{workflowRid}/publish", async (string workflowRid,
            PublishProjectionReq request, IZLinkSpotClient routes,
            CancellationToken cancellationToken) =>
        {
            var response = await routes.RequestToSpot(workflowRid, request)
                .Async<PublishProjectionRes>(cancellationToken);
            return Results.Ok(response);
        });
        app.MapGet("/evidence", async (
            IZLinkFrameworkRuntime runtime,
            IZLinkLocationRuntimeQuery locations,
            IZLinkSpotManager spots,
            WorkflowEvidenceStore evidence,
            MetricEvidenceCollector metrics,
            string? spotRid) =>
        {
            // The evidence endpoint uses only the public topology and Spot
            // resolver surfaces. It must still answer while Redis is paused.
            IReadOnlyList<ZLinkLocationTopologyEntry> peers;
            try
            {
                peers = (await locations.ListTopologyAsync(
                            new ZLinkLocationTopologyFilter(ObservabilityNames.WorkflowMesh))
                        .AsTask().WaitAsync(TimeSpan.FromMilliseconds(500)))
                    .Items;
            }
            catch (Exception)
            {
                peers = [];
            }
            var spotRows = Array.Empty<SpotRow>();
            if (!string.IsNullOrWhiteSpace(spotRid))
            {
                try
                {
                    if (await spots.FindAsync(spotRid)
                            .AsTask().WaitAsync(TimeSpan.FromMilliseconds(500))
                        is { } spot)
                        spotRows =
                        [
                            new SpotRow(
                                spot.MeshName,
                                spot.NodeRid.ToString(),
                                spot.SpotId,
                                "spot",
                                checked((long)spot.ObjectGeneration))
                        ];
                }
                catch (Exception)
                {
                    // Resolve-only observation degrades with the store.
                }
            }

            return Results.Ok(new EvidenceSnapshot(options.Rid, runtime.Status.IsReady, evidence.Snapshot(),
                metrics.Snapshot(), peers.Select(row => new PeerRow(row.NodeRid.ToString(),
                    row.Draining, row.UpdatedAt.UtcTicks)).ToArray(), [],
                spotRows));
        });
        app.MapPost("/evidence/wait", async (EvidenceWaitReq request, WorkflowEvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var entries = await evidence.WaitAsync(snapshot => Matches(snapshot, request),
                TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)), cancellationToken);
            return Results.Ok(entries);
        });
        app.MapPost("/metrics/wait", async (MetricWaitReq request, MetricEvidenceCollector metrics,
            CancellationToken cancellationToken) => Results.Ok(await metrics.WaitAsync(
            samples => MetricWait.Matches(samples, request),
            TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)),
            cancellationToken)));
        app.MapMaintenanceOperations();
        return app;
    }

    private static bool Matches(string[] entries, EvidenceWaitReq request) =>
        request.ContainsAll.All(expected => entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal)))
        && request.ContainsAnyGroups.All(group => group.Any(expected =>
            entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal))));

}
