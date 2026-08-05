using RegistrationCodec.Server.JsonOnlyPeer;

var app = RegistrationCodecServerHostFactory.Create(args);
await app.RunAsync();