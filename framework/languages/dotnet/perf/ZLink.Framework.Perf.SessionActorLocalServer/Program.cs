using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.scenario != "cs-local-session-actor-echo" || config.role != "session-actor-local")
    throw new ArgumentException("SessionActorLocalServer runs the cs-local-session-actor-echo role.");
// §10.1: the STREAM session and the Actors it binds live on this one Object Server node.
var builder = ServerApplication.Builder(config, options =>
{
    options.AddRouteMesh(config.meshName!).Listen(config.transportEndpoints["mesh"]).Objects().Server().AddPerfActors();
    options.AddStreamNode("perf-session").Bind(config.transportEndpoints["stream"]).EnableActorDispatch().AddSession<PerfActorRelaySession>();
});
builder.Services.AddSingleton(new ObjectsReadiness(false, "No Actor is bound to a session yet."));
builder.Services.AddSingleton<SessionActorSetup>();
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
