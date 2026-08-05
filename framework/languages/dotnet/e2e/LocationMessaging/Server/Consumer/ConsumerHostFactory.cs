using Microsoft.Extensions.Configuration;
using Systems.Zlink;

using Microsoft.AspNetCore.Builder;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Locations.Redis;
using LocationMessaging.Server.Consumer.Configuration;
using LocationMessaging.Server.Consumer.Endpoints;
using LocationMessaging.Shared;

namespace LocationMessaging.Server.Consumer;

internal static class ConsumerHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ConsumerOptions.Parse(args);
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
        builder.WebHost.ConfigureKestrel(server =>
        {
            // RM-C8 sends a 33 MiB JSON request to exercise the Framework
            // transport bound, so HTTP admission must not reject it first.
            server.Limits.MaxRequestBodySize = 64L * 1024 * 1024;
        });

        builder.WebHost.UseUrls(options.HttpUrl);
        builder.Services.AddSingleton(options);
        builder.Services.AddSingleton<ConnectionEvidence>();
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            var profileMesh = framework.AddRouteMesh(options.MeshName);
            if (string.IsNullOrWhiteSpace(options.MeshEndpoint))
            {
                profileMesh.Listen()
                    .SetBindHost("127.0.0.1")
                    .SetAdvertiseHost("127.0.0.1");
            }
            else
            {
                profileMesh.Listen(options.MeshEndpoint);
            }
            switch (options.RouteChannelRole)
            {
                case "Client":
                    profileMesh.Channel("profile").Client();
                    break;
                case "Server":
                    profileMesh.Channel("profile").Server()
                        .SetWeight(options.RouteChannelWeight);
                    break;
                case "None":
                    break;
                default:
                    throw new InvalidOperationException(
                        $"Unknown RouteChannelRole '{options.RouteChannelRole}'.");
            }
            switch (options.ObjectRole)
            {
                case "None":
                    break;
                case "Client":
                    profileMesh.Objects().Client();
                    break;
                case "Server":
                    profileMesh.Objects().Server();
                    break;
                default:
                    throw new InvalidOperationException(
                        $"Unknown ObjectRole '{options.ObjectRole}'.");
            }

            if (options.RegisterIndependentTopologies)
            {
                // These registrations use separate physical topologies. They
                // must not make an Object Client pair require RouteMesh peers.
                framework.AddClientServerChannel("rm-a3-client-server").Client();
                framework.AddFanoutChannel("rm-a3-fanout")
                    .EnableSubscriber()
                    .AddHandler<RmA3FanoutProbeHandler, ProfileMsg>();
            }

            var manualEndpoints = options.ProviderEndpoints ?? [];
            if (!string.IsNullOrWhiteSpace(options.RedisEndpoint))
            {
                framework.AddLocationStore(new ZLinkRedisLocationStore(redis =>
                {
                    redis.ConnectionString = options.RedisEndpoint;
                    redis.KeyPrefix = options.RedisKeyPrefix!;
                }));
                if (manualEndpoints.Count == 0)
                {
                    // Automatic discovery allocates the RID from this prefix.
                    profileMesh.SetRoutingIdPrefix($"00-{options.TraceLabel}");
                }
            }

            if (options.RegisterWorkflowClient)
            {
                if (string.IsNullOrWhiteSpace(options.RedisEndpoint))
                    throw new InvalidOperationException(
                        "RegisterWorkflowClient requires a shared Redis location store.");

                var workflowMesh = framework.AddRouteMesh("workflow")
                    .Listen()
                    .SetBindHost("127.0.0.1")
                    .SetAdvertiseHost("127.0.0.1")
                    .SetRoutingIdPrefix($"00-{options.TraceLabel}-workflow");
                workflowMesh.Channel("workflow").Client();
            }

            if (manualEndpoints.Count > 0)
            {
                if (string.Equals(options.ObjectRole, "None", StringComparison.Ordinal))
                    profileMesh.SetRoutingId(RoutingId.From(options.TraceLabel));
                foreach (var endpoint in manualEndpoints)
                    profileMesh.PeerConnections.Connect(endpoint);
            }

        });
        builder.Services.AddHostedService<MeshConnectionEventObserver>();
        var app = builder.Build();
        app.MapConsumerEndpoints(options);
        return app;
    }
}

internal sealed class RmA3FanoutProbeHandler : IZLinkFanoutHandler<ProfileMsg>
{
    public ValueTask HandleAsync(
        ProfileMsg message,
        CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}
