using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.role != "actor") throw new ArgumentException("ActorServer runs the actor role.");
// Skeleton: no scenario handler is registered yet, so the role reports objectsReady=false instead of pretending.
var builder = ServerApplication.Builder(config, _ => { });
builder.Services.AddSingleton(new ObjectsReadiness(false, "ActorServer has no scenario handlers yet."));
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
