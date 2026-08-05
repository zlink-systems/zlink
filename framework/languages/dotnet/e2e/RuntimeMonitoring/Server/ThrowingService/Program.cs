using RuntimeMonitoring.Server.ThrowingService;

var app = ThrowingServiceHostFactory.Create(args);
await app.RunAsync();
