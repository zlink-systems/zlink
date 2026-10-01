using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.role != "session" || config.source || config.scenario is not ("session-echo-only" or "cs-remote-session-actor-echo"))
    throw new ArgumentException("SessionServer supports the session receiver roles of §10.2 and §11.1.");
var builder = ServerApplication.Builder(config, options =>
{
    if (config.scenario == "session-echo-only")
        options.AddStreamNode("perf-session").Bind(config.transportEndpoints["stream"]).AddSession<PerfSession>();
    else
    {
        // §10.2: an Object Client node; the Actors live in the separate Actor process.
        options.AddRouteMesh(config.meshName!).Listen(config.transportEndpoints["mesh"]).Objects().Client();
        options.AddStreamNode("perf-session").Bind(config.transportEndpoints["stream"]).EnableActorDispatch().AddSession<PerfActorRelaySession>();
    }
});
if (config.scenario == "cs-remote-session-actor-echo")
{
    builder.Services.AddSingleton(new ObjectsReadiness(true, ""));
    builder.Services.AddSingleton<SessionActorSetup>();
}
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
