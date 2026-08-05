using ObservabilityOps.Server.Session;

var app = SessionHostFactory.Create(args);
await app.RunAsync();
