using Systems.Zlink;
using Microsoft.Extensions.Configuration;

using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using SupportChat.Server.Configuration;
using SupportChat.Server.Support.Application.ConversationAssignment;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot;
using SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot.Notifications;
using SupportChat.Server.Support.Infrastructure.ZLink.Spots.EntrySpot;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Samples.Logging;

namespace SupportChat.Server.Support;

public static class SupportServerHostFactory
{
    public static IHost Build(SampleTopology topology, string logDirectory)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            logDirectory,
            "support");
        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton(new AgentAvailabilityDirectory(SampleNames.AgentCapacity));
        builder.Services.AddSingleton<AgentAssignmentService>();
        builder.Services.AddSingleton<SupportActorDirectory>();
        builder.Services.AddSingleton<ConversationNotificationPublisher>();

        builder.Services.AddZLinkFramework(options =>
        {
            options.DefaultRequestTimeout = TimeSpan.FromSeconds(15);
            var locations = options.ConfigureLocations();
            locations.RouteCacheMaxAge = TimeSpan.Zero;
            locations.MessageFollowDuration = TimeSpan.FromSeconds(5);
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = $"{topology.RedisKeyPrefix}relocation:";
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.ConfigureMetadata()
                .AllowSessionToActor(SampleNames.ConversationIdMetadataKey)
                .AllowActorToSession(SampleNames.ConversationIdMetadataKey);
            options.AddHandlersFromAssemblyOf(typeof(SupportServerHostFactory));
            var mesh = options.AddRouteMesh(SampleNames.MeshName)
                .Listen(topology.MeshEndpoint)
                .SetRoutingIdPrefix("support-owner");
            mesh.Objects().Server()
                .AddEntrySpot<SupportEntrySpot>()
                .AddActorFactory<SupportUserActor, SupportUserActorFactory>(
                    SampleNames.SupportActorType, factory => factory.PreserveStateWith<SupportUserActorRelocationAdapter>())
                .AddSpotFactory<ConversationSpot>(
                    SampleNames.ConversationSpotType, factory => factory.DisableRelocation());
            options.AddClientServerChannel(SampleNames.ApiChannel).Client();
        });

        return builder.Build();
    }
}
