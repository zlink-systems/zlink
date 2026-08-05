using Microsoft.Extensions.Configuration;

using ObservabilityOps.Server.Session.Sessions;
using ObservabilityOps.Server.Session.Support;
using ObservabilityOps.Server.Support;
using ObservabilityOps.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace ObservabilityOps.Server.Session;

internal static class SessionHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ObservabilityOps.Server.Session.Support.SessionOptions.Parse(args);
        Directory.CreateDirectory(options.LogDir);
        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console => console.SingleLine = true);
        builder.WebHost.UseUrls(options.HttpUrl);
        builder.Services.AddSingleton(options);
        builder.Services.AddSingleton<MetricEvidenceCollector>();
        builder.Services.AddSingleton<RelocationOperation>();
        builder.Services.AddSingleton<ShutdownOperation>();
        builder.Services.AddSingleton<BoundedOperationGate>();
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"flow-{options.Rid}.log"),
            options.Rid));
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            framework.AddHandlersFromAssemblyOf(typeof(SessionHostFactory));
            var mesh17 = framework.AddRouteMesh(ObservabilityNames.PlayMesh)
                .Listen(options.RouterEndpoint);
            mesh17.Objects().Client();
            mesh17.Channel(ObservabilityNames.PlayMesh).Client();
            framework.AddStreamNode(ObservabilityNames.StreamNode)
                .Bind(options.StreamEndpoint)
                .EnableActorDispatch()
                .AddSession<ObservabilitySession>();
        });
        var app = builder.Build();
        _ = app.Services.GetRequiredService<MetricEvidenceCollector>();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Rid }));
        app.MapDiagnosticsControl();
        app.MapGet("/evidence", async (
            MetricEvidenceCollector metrics,
            IZLinkFrameworkRuntime runtime,
            IZLinkLocationRuntimeQuery locations) =>
        {
            var peers = await locations.ListTopologyAsync(
                new ZLinkLocationTopologyFilter(ObservabilityNames.PlayMesh));
            return Results.Ok(new EvidenceSnapshot(
                options.Rid, runtime.Status.IsReady, [], metrics.Snapshot(),
                peers.Items.Select(row => new PeerRow(row.NodeRid.ToString(),
                    row.Draining, row.UpdatedAt.UtcTicks)).ToArray(), [], []));
        });
        app.MapPost("/metrics/wait", async (MetricWaitReq request, MetricEvidenceCollector metrics,
            CancellationToken cancellationToken) => Results.Ok(await metrics.WaitAsync(
            samples => MetricWait.Matches(samples, request),
                TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)),
                cancellationToken)));
        app.MapMaintenanceOperations();
        app.MapBoundedOperationGate();
        return app;
    }
}
