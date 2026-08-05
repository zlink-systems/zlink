using Microsoft.Extensions.Configuration;

using Bingo.Server.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Samples.Logging;

namespace Bingo.Server.Api;

public static class ApiServerHostFactory
{
    public static IHost Build(
        SampleRuntimeConfiguration<SampleApiNode> configuration)
    {
        var node = configuration.Node;
        var nodeName = configuration.NodeName;
        var logDirectory = configuration.LogDirectory;
        var traceLabel = $"api-{nodeName}";
        var builder = Host.CreateApplicationBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            logDirectory,
            traceLabel);
        builder.Services.AddSingleton(configuration);
        builder.Services.AddSingleton<BingoPlayerRecordStore>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = configuration.RedisEndpoint;
                redis.KeyPrefix = configuration.RedisKeyPrefix;
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(ApiServerHostFactory));
            options.Codecs.Use(ZLinkProtobufCodec.Default);
            options.AddRouteMesh(SampleNames.PlayMeshName)
                .SetRoutingIdPrefix("api")
                .Listen(node.PlayMeshEndpoint)
                .Objects().Client();
            options.AddRouteMesh(SampleNames.MatchmakingMeshName)
                .SetRoutingIdPrefix("api-matchmaking")
                .Listen(node.MatchmakingMeshEndpoint)
                .Objects().Client();
            options.AddClientServerChannel(SampleNames.ApiChannel).Server()
                .Listen()
                .AddHandlerGroup("api");
        });
        builder.Services.AddSingleton(new BingoMeshStatusReport(
            "api",
            SampleNames.PlayMeshName));
        builder.Services.AddHostedService<BingoMeshStatusReporter>();
        return builder.Build();
    }
}
