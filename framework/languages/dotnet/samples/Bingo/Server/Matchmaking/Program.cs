using Bingo.Server.Configuration;
using Bingo.Server.Matchmaking;
using Microsoft.Extensions.Hosting;

var configuration = SampleConfigurationLoader.LoadMatchmaking(args);
await MatchmakingServerHostFactory.Build(configuration).RunAsync();
