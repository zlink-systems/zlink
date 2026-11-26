using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Api;
using MyGame.Shared;
using StackExchange.Redis;

var settings = NodeSettings.Load(args);
var endpoint = new Uri(
    settings.ApiEndpoint ?? throw new InvalidOperationException("ApiEndpoint is required.")
);
var builder = Host.CreateApplicationBuilder();
builder.Services.AddSingleton<IConnectionMultiplexer>(_ =>
    ConnectionMultiplexer.Connect(settings.RedisEndpoint)
);
builder.Services.AddSingleton<RedisPlayerRecords>();
GameHosting.Configure(
    builder,
    settings,
    options =>
        options
            .AddClientServerChannel(PlayerRecords.Channel)
            .Server()
            .SetBindHost(endpoint.Host)
            .SetAdvertiseHost(endpoint.Host)
            .Listen(endpoint.Port)
            .AddRequestHandler<LoadPlayerHandler, LoadPlayer, PlayerRecord>()
            .AddRequestHandler<CommitRoundHandler, CommitRound, RoundCommitted>()
);
using var host = builder.Build();
_ = host.Services.GetRequiredService<RedisPlayerRecords>();
await NodeControl.RunAsync(host, settings);
