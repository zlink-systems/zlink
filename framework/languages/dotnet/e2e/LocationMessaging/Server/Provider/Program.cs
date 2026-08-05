using LocationMessaging.Server.Provider;

var app = ProviderHostFactory.Create(args);
await app.RunAsync();