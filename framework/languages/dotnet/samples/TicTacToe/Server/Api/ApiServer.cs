using Microsoft.Extensions.Configuration;

using Systems.Zlink;
using TicTacToe.Server.Api.Handlers;
using TicTacToe.Server.Configuration;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using Zlink.Samples.Logging;

namespace TicTacToe.Server.Api;

internal sealed class ApiServer(SampleSettings settings)
{
    public WebApplication Build()
    {
        var builder = WebApplication.CreateBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(builder.Logging, settings.LogDirectory, "api");
        builder.WebHost.UseUrls(settings.ApiBindUrl);
        builder.Services.AddSingleton(settings);
        builder.Services.AddSingleton(new TicTacToeMeshReadiness(
            TicTacToeReadyKind.SpotRoute,
            settings.InstanceName,
            SampleNodes.Mesh));
        builder.Services.AddHostedService<TicTacToeMeshReadinessReporter>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = settings.RedisEndpoint;
                redis.KeyPrefix = settings.RedisKeyPrefix;
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);

            var apiChannelEndpoint = new Uri(
                settings.ApiChannelListenEndpoint,
                UriKind.Absolute);
            options.AddClientServerChannel(SampleChannels.Api)
                .Server()
                .Listen(apiChannelEndpoint.Port)
                .SetBindHost(apiChannelEndpoint.Host)
                .SetAdvertiseHost(apiChannelEndpoint.Host)
                // request: authenticates a Play session through the API role.
                .AddRequestHandler<AuthenticatePlayerHandler, AuthenticatePlayerReq, AuthenticatePlayerRes>();

                // Spec 10.1 wants a fixed RID here so the expected peer can be named. .NET cannot
                // yet: ZLinkSpotNodeInitializer.RequiresDescriptorClaim skips the object descriptor
                // claim when a routing ID is explicit, so an object-role node with a fixed RID is
                // never matched as a User Spot target. Tracked in
                // doc/plan/spec-server-reorg/spec-gap.ko.md; revert to automatic RID until fixed.
            var mesh = options.AddRouteMesh(SampleNodes.Mesh)
                .SetRoutingId(SampleNodes.RouteMeshRoutingId(settings.InstanceName))
                .Listen(settings.MeshEndpoint);
            mesh.Objects().Client();
            // PeerMeshEndpoints is [play-a, play-b] (see run_sample.sh), so index maps to node id.
            for (var index = 0; index < settings.PeerMeshEndpoints.Count; index++)
                mesh.PeerConnections.Connect(
                    SampleNodes.RouteMeshRoutingId(index == 0 ? "play-a" : "play-b"),
                    settings.PeerMeshEndpoints[index]);
        });

        var app = builder.Build();
        app.MapPost("/games", CreateGameHttpHandler.HandleAsync);
        app.Lifetime.ApplicationStarted.Register(() =>
            app.Logger.LogInformation("tictactoe-ready kind=http node={NodeId}", settings.InstanceName));
        return app;
    }
}
