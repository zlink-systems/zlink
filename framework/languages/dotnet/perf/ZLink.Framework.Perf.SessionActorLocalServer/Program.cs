using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.role != "session-actor-local") throw new ArgumentException("SessionActorLocalServer runs the session-actor-local role.");
// Skeleton: no scenario handler is registered yet, so the role reports objectsReady=false instead of pretending.
var builder = ServerApplication.Builder(config, _ => { });
builder.Services.AddSingleton(new ObjectsReadiness(false, "SessionActorLocalServer has no scenario handlers yet."));
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
