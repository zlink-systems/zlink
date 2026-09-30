using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.scenario != "pubsub-fanout-echo" || config.role != "subscriber" || config.source)
    throw new ArgumentException("SubscriberServer runs the subscriber role of pubsub-fanout-echo.");
// Automatic Classic fanout: no endpoint and no Subscribe(topic), so this subscriber receives every topic (§10.11).
var builder = ServerApplication.Builder(config, options => options.AddFanoutChannel(config.channelName!)
    .EnableSubscriber().AddHandler<PerfFanoutHandler, PerfPublishEvent>());
builder.Services.AddSingleton(new ObjectsReadiness(false, "No Ready publisher is visible to this Subscriber yet."));
// role-configs/<role>.json sits one folder below the cell directory (perf §15.1), where the sequence original is written.
var cellDirectory = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(args[1])))!;
builder.Services.AddSingleton(sp => ActivatorUtilities.CreateInstance<FanoutReceipts>(sp, cellDirectory));
var app = builder.Build();
var receipts = app.Services.GetRequiredService<FanoutReceipts>();
ServerApplication.Map(app);
await app.StartAsync();
await receipts.PrepareAsync(app.Lifetime.ApplicationStopping);
await app.WaitForShutdownAsync();
