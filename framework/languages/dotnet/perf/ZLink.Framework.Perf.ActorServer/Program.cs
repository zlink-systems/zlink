using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.role != "actor" || config.scenario is not ("cs-remote-session-actor-echo" or "actor-no-bind-request-echo" or "actor-no-bind-send-send-echo"))
    throw new ArgumentException("ActorServer runs the actor role of §10.2, §10.9 and §10.10.");
// The Actor Object Server hosts the Actors that a remote Session binds or an ActorCaller addresses by ActorId.
var builder = ServerApplication.Builder(config, options =>
{
    var mesh = options.AddRouteMesh(config.meshName!).Listen(config.transportEndpoints["mesh"]);
    // §10.10: the Actor answers through the public Channel client; the caller is the return channel Server.
    if (config.mode == "send-send") mesh.Channel(config.channelName!).Client();
    mesh.Objects().Server().AddPerfActors();
});
builder.Services.AddSingleton(new ObjectsReadiness(false, "No typed probe has reached an Actor yet."));
builder.Services.AddHostedService<ActorPlacementWatcher>();
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
