using RegistrationCodec.Server.CodecRequester;

var app = CodecRequesterHostFactory.Create(args);
await app.RunAsync();
