using Microsoft.Extensions.Configuration;

using LocationMessaging.Server.Provider.Configuration;
using LocationMessaging.Server.Provider.Endpoints;
using LocationMessaging.Server.Provider.Handlers;
using LocationMessaging.Server.Provider.Infrastructure;
using LocationMessaging.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace LocationMessaging.Server.Provider;

internal static class ProviderHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ServerOptions.Parse(args, "provider");
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
        builder.Services.AddSingleton<BackpressureGate>();
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid,
            flow =>
            {
                if (flow.Phase is not ("error" or "dropped")) return;
                evidence.Add(
                    "dispatch-error"
                    + $"|surface={flow.Surface}"
                    + $"|kind={flow.MessageKind}"
                    + $"|reason={flow.Reason}"
                    + $"|action={flow.Action}"
                    + $"|packet={flow.PacketName ?? "<null>"}"
                    + $"|channel={flow.ChannelName ?? "<null>"}");
            }));
        if (!string.IsNullOrWhiteSpace(options.ChannelEndpoint))
            builder.Services.AddHostedService<ProfileMeshEventObserver>();

        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            var inboundDispatch = framework.ConfigureInboundDispatch();
            inboundDispatch.ProcessMemoryLimitBytes = 1UL * 1024 * 1024 * 1024;
            if (options.ApplicationHwmBytes > 0)
                inboundDispatch.ApplicationHwmBytes = options.ApplicationHwmBytes;
            // The official Redis extension registers the peer/spot/actor/route
            // stores and the owner lease store together (doc §2).
            framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint
                                         ?? throw new InvalidOperationException(
                                             "Shared.RedisEndpoint is required."); redis.KeyPrefix = options.RedisKeyPrefix
                                  ?? throw new InvalidOperationException(
                                      "Shared.RedisKeyPrefix is required."); }));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);

            if (!string.IsNullOrWhiteSpace(options.ChannelEndpoint))
            {
                var profileMesh = framework.AddRouteMesh("profile")
                    .Listen(options.ChannelEndpoint)
                    .SetRoutingIdPrefix(options.Rid);
                var profile = profileMesh.Channel("profile").Server().SetWeight(options.Weight);
                var serverSocket = profileMesh.ConfigureRouterSocket();
                serverSocket.ReceiveHighWaterMark = 4;
                profile.AddRequestHandler<ProfileRequestHandler, ProfileReq, ProfileRes>("ProfileReq");
                profile.AddRequestHandler<PayloadRequestHandler, PayloadReq, PayloadRes>("PayloadReq");
                profile.AddSendHandler<ProfileCommandHandler, ProfileMsg>("ProfileMsg");
                profile.AddSendHandler<BackpressureCommandHandler, BackpressureMsg>("BackpressureMsg");
            }

            if (!string.IsNullOrWhiteSpace(options.RouteEndpoint))
            {
                var route = framework.AddRouteMesh("profile.route")
                    .Listen(options.RouteEndpoint);
                if (options.RoutePeers is { Count: > 0 })
                {
                    route.SetRoutingId(RoutingId.From(options.Rid));
                    foreach (var peer in options.RoutePeers) route.PeerConnections.Connect(peer);
                }
                else
                {
                    route.SetRoutingIdPrefix($"{options.Rid}-route");
                }

                route.AddRouteRequestHandler<RoutePingHandler, ScenarioRoutePing, ScenarioRoutePong>("ScenarioRoutePing");
            }
        });
        var app = builder.Build();
        app.MapProviderEndpoints(options);
        return app;
    }
}
