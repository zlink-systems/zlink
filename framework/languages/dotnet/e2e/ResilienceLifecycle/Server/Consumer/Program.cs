using ResilienceLifecycle.Server.Consumer;

var app = ConsumerHostFactory.Create(args);
await app.RunAsync();
