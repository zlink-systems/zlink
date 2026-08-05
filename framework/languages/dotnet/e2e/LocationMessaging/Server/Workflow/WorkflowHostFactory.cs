using Microsoft.Extensions.Configuration;

using Microsoft.AspNetCore.Builder;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using LocationMessaging.Server.Workflow.Configuration;
using LocationMessaging.Server.Workflow.Endpoints;
using LocationMessaging.Server.Workflow.Handlers;
using LocationMessaging.Server.Workflow.Infrastructure;
using LocationMessaging.Shared;
using Systems.Zlink;
using Zlink.Framework;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace LocationMessaging.Server.Workflow;

internal static class WorkflowHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ServerOptions.Parse(args, defaultRole: "workflow");
        Directory.CreateDirectory(options.LogDir);

        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console =>
        {
            console.SingleLine = true;
            console.TimestampFormat = "HH:mm:ss.fff ";
        });

        builder.WebHost.UseUrls(options.HttpUrl);
        var evidence = new EvidenceStore(options.Rid, options.EvidenceFile);
        builder.Services.AddSingleton(evidence);
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid,
            flow =>
            {
                if (flow.Phase != "error") return;
                evidence.Add(
                    "dispatch-error"
                    + $"|surface={flow.Surface}"
                    + $"|kind={flow.MessageKind}"
                    + $"|reason={flow.Reason}"
                    + $"|action={flow.Action}"
                    + $"|packet={flow.PacketName ?? "<null>"}"
                    + $"|channel={flow.ChannelName ?? "<null>"}");
            }));

        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            // The official Redis extension registers the peer/spot/actor/route
            // stores and the owner lease store together (doc §2).
            framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint
                    ?? throw new InvalidOperationException("Shared.RedisEndpoint is required."); redis.KeyPrefix = options.RedisKeyPrefix
                    ?? throw new InvalidOperationException("Shared.RedisKeyPrefix is required."); }));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);

            var mesh = framework.AddRouteMesh("workflow")
                .Listen(options.WorkflowEndpoint)
                .SetRoutingIdPrefix(options.Rid);
            var workflow = mesh.Channel("workflow").Server().SetWeight(options.Weight);
            workflow.AddRequestHandler<WorkflowRequestHandler, WorkflowReq, WorkflowRes>("WorkflowReq");
        });

        var app = builder.Build();
        app.MapWorkflowEndpoints(options);
        return app;
    }
}
