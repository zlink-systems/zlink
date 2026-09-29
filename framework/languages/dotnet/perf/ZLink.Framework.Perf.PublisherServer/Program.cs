using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.role != "publisher") throw new ArgumentException("PublisherServer runs the publisher role.");
// Skeleton: no scenario handler is registered yet, so the role reports objectsReady=false instead of pretending.
var builder = ServerApplication.Builder(config, _ => { });
builder.Services.AddSingleton(new ObjectsReadiness(false, "PublisherServer has no scenario handlers yet."));
var app = builder.Build();
ServerApplication.Map(app);
await app.RunAsync();
