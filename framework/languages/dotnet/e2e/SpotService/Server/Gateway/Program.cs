using SpotService.Server.Gateway;

var app = GatewayHostFactory.Create(args);
await app.RunAsync();
