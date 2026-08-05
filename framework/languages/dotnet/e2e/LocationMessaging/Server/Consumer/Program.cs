using LocationMessaging.Server.Consumer.Configuration;
using LocationMessaging.Server.Consumer.Endpoints;
using LocationMessaging.Server.Consumer;

var app = ConsumerHostFactory.Create(args);
await app.RunAsync();
