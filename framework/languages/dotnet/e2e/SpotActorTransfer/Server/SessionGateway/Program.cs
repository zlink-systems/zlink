using SpotActorTransfer.SessionGateway;

var app = SessionGatewayHostFactory.Create(args);
await app.RunAsync();
