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

            var mesh = options.AddRouteMesh(SampleNodes.Mesh)
                .Listen(settings.MeshEndpoint);
            mesh.Objects().Client();
            foreach (var endpoint in settings.PeerMeshEndpoints)
                mesh.PeerConnections.Connect(endpoint);
        });

        var app = builder.Build();
        app.MapPost("/games", CreateGameHttpHandler.HandleAsync);
        return app;
    }
}
