using SpotService.Server.MultiNode;

var app = MultiNodeHostFactory.Create(args);
await app.RunAsync();