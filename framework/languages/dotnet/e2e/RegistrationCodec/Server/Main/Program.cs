using RegistrationCodec.Server.Main;

var app = RegistrationCodecServerHostFactory.Create(args);
await app.RunAsync();