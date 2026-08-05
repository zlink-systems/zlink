using RegistrationCodec.Server.InvalidDuplicate;

var app = RegistrationCodecServerHostFactory.Create(args);
await app.RunAsync();