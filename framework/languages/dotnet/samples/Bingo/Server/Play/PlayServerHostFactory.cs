using Microsoft.Extensions.Configuration;

using Bingo.Server.Configuration;
using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Notifications;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.EntrySpot;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Samples.Logging;

namespace Bingo.Server.Play;

public static class PlayServerHostFactory
{
    public static IHost Build(
        SampleRuntimeConfiguration<SamplePlayNode> configuration,
        bool enableMetrics = true)
    {
        var node = configuration.Node;
        var nodeName = configuration.NodeName;
        var logDirectory = configuration.LogDirectory;
        var traceLabel = $"play-{nodeName}";
        var builder = Host.CreateApplicationBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            logDirectory,
            traceLabel);
        builder.Services.AddSingleton(configuration);
        builder.Services.AddSingleton(node);
        builder.Services.AddSingleton<BingoRoomEventMapper>();
        builder.Services.AddSingleton<BingoNotificationPublisher>();
        if (enableMetrics) builder.Services.AddBingoMetrics();

        builder.Services.AddZLinkFramework(options =>
        {
            options.DefaultRequestTimeout = TimeSpan.FromSeconds(15);
            var locations = options.ConfigureLocations();
            locations.RouteCacheMaxAge = TimeSpan.Zero;
            locations.MessageFollowDuration = TimeSpan.FromSeconds(5);
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
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(PlayServerHostFactory));
            options.Codecs.Use(ZLinkProtobufCodec.Default);
            var mesh = options.AddRouteMesh(SampleNames.PlayMeshName)
                .SetRoutingIdPrefix("play")
                .Listen(node.MeshEndpoint);
            mesh.Objects().Server()
                .AddEntrySpot<BingoEntrySpot>()
                .AddActorFactory<PlayerActor, PlayerActorFactory>(
                    SampleNames.PlayerActorType,
                    factory => factory
                        .PreserveStateWith<PlayerActorRelocationAdapter>())
                .AddSpotFactory<BingoRoom>(
                    SampleNames.RoomSpotType,
                    factory => factory
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .RelocationReadiness(
                            ZLinkSpotRelocationReadinessMode.ApplicationSignaled)
                        .PreserveStateWith<BingoRoomRelocationAdapter>());
            mesh.Channel(SampleNames.RoomChannel).Server();
            options.AddClientServerChannel(SampleNames.ApiChannel).Client();
        });
        builder.Services.AddSingleton(new BingoMeshStatusReport(
            "play",
            SampleNames.PlayMeshName));
        builder.Services.AddHostedService<BingoMeshStatusReporter>();
        return builder.Build();
    }
}
