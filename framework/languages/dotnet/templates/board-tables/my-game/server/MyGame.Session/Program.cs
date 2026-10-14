using Microsoft.Extensions.Hosting;
using MyGame.Session;
using MyGame.Shared;

var settings = NodeSettings.Load(args);
using var flow = new MessageFlow();
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf(typeof(PlayerSession));
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Client();
        options
            .AddStreamNode(GameNames.SessionStream)
            .Bind(
                settings.StreamEndpoint
                    ?? throw new InvalidOperationException("StreamEndpoint is required.")
            )
            .EnableActorDispatch()
            .AddSession<PlayerSession>();
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
