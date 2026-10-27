using Microsoft.Extensions.Hosting;
using MyGame.Session;
using MyGame.Shared;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint);
        options
            .AddStreamNode(GameNames.SessionStream)
            .Bind(
                settings.StreamEndpoint
                    ?? throw new InvalidOperationException("StreamEndpoint is required.")
            )
            .AddSession<PlayerSession>();
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
