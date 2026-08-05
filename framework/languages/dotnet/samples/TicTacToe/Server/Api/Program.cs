using TicTacToe.Server.Api;
using TicTacToe.Server.Configuration;

var settings = SampleSettings.LoadApi(args);
await using var server = new ApiServer(settings).Build();
await server.RunAsync();
