using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.scenario != "session-echo-only" || config.role != "session" || config.source)
    throw new ArgumentException("SessionServer supports the session-echo-only receiver role.");
var builder = ServerApplication.Builder(config, options =>
    options.AddStreamNode("perf-session").Bind(config.listenerEndpoint!).AddSession<PerfSession>());
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
