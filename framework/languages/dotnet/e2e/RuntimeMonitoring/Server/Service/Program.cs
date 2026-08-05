using RuntimeMonitoring.Server.Service;

var app = ServiceHostFactory.CreateAll(args);
await app.RunAsync();