using SpotActorTransfer.ActorNode;

var (app, options) = ActorNodeHostFactory.Create(args);
ActorNodeEndpoints.Map(app, options);
await app.RunAsync();
