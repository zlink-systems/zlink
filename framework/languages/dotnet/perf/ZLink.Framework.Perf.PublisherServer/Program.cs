using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.scenario != "pubsub-fanout-echo" || config.role != "publisher" || !config.source)
    throw new ArgumentException("PublisherServer runs the publisher role of pubsub-fanout-echo.");
// Automatic Classic fanout: the publisher listens on the reserved endpoint and publishes its descriptor to the run's Store.
var builder = ServerApplication.Builder(config, options =>
    options.AddFanoutChannel(config.channelName!).EnablePublisher(config.transportEndpoints["fanout"]));
builder.Services.AddSingleton(new ObjectsReadiness(false, "The Publisher host is not Ready yet."));
// role-configs/<role>.json sits one folder below the cell directory (perf §15.1), where the sequence original is written.
var cellDirectory = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(args[1])))!;
builder.Services.AddSingleton(sp => ActivatorUtilities.CreateInstance<PubSubFanoutEchoScenario>(sp, cellDirectory));
var app = builder.Build();
var scenario = app.Services.GetRequiredService<PubSubFanoutEchoScenario>();
ServerApplication.Map(app, scenario.RunAsync);
await app.StartAsync();
await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
await app.WaitForShutdownAsync();
