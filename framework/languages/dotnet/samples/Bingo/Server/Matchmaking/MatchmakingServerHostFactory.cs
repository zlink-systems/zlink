using Bingo.Server.Configuration;
using Bingo.Server.Matchmaking.Application;
using Bingo.Server.Matchmaking.Infrastructure.Redis;
using Bingo.Server.Matchmaking.Infrastructure.ZLink;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using StackExchange.Redis;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using Zlink.Samples.Logging;

namespace Bingo.Server.Matchmaking;

public static class MatchmakingServerHostFactory
{
    public static IHost Build(
        SampleRuntimeConfiguration<SampleMatchmakingNode> configuration)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Services.AddSingleton(configuration);
        builder.Services.AddSingleton<IConnectionMultiplexer>(_ =>
            ConnectionMultiplexer.Connect(configuration.RedisEndpoint));
        builder.Services.AddSingleton<IBingoMatchReservationStore,
            RedisBingoMatchReservationStore>();
        SampleLogging.Configure(builder.Logging, configuration.LogDirectory, "matchmaking");
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = configuration.RedisEndpoint;
                redis.KeyPrefix = configuration.RedisKeyPrefix;
            }));
            options.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
            {
                redis.ConnectionString = configuration.RedisEndpoint;
                redis.KeyPrefix = $"{configuration.RedisKeyPrefix}relocation:";
            }));
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(MatchmakingServerHostFactory));
            options.Codecs.Use(ZLinkProtobufCodec.Default);
            options.AddRouteMesh(SampleNames.MatchmakingMeshName)
                .SetRoutingIdPrefix("matchmaking")
                .Listen(configuration.Node.MeshEndpoint)
                .Objects().Server()
                .AddInstanceSpotFactory<BingoMatchmaker>(
                    SampleNames.MatchmakerSpotType, factory => factory.RecreateOnRelocation());
        });
        return builder.Build();
    }
}
