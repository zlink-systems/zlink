using RuntimeMonitoring.Server.FilteredService;

var app = FilteredServiceHostFactory.Create(args);
await app.RunAsync();
