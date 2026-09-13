using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (!config.scenario.StartsWith("cs-", StringComparison.Ordinal) && config.scenario != "session-echo-only" || config.source)
    throw new ArgumentException("SessionServer supports the session-echo-only receiver role.");
var builder = ServerApplication.Builder(config, options =>
{
    var stream = options.AddStreamNode("perf-session").Bind(config.listenerEndpoint!).AddSession<PerfSession>();
    if (config.scenario != "session-echo-only") { stream.EnableActorDispatch(); PerfScenario.Configure(options, config); }
});
builder.Services.AddSingleton<PerfScenario>();
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
