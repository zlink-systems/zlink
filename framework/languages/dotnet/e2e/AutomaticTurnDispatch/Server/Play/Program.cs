using AutomaticTurnDispatch.Server.Play;

var app = PlayHostFactory.Create(args);
await app.RunAsync();