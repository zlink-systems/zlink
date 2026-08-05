using Microsoft.Extensions.Configuration;

using Systems.Zlink;
using TicTacToe.Server.Configuration;
using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Server.Play.Infrastructure.ZLink.Sessions;
using TicTacToe.Server.Play.Infrastructure.ZLink.Spots.EntrySpot;
using TicTacToe.Server.Play.Infrastructure.ZLink.Spots.TicTacToeGameSpot;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using Zlink.Samples.Logging;

namespace TicTacToe.Server.Play;

internal sealed class PlayServer(SampleSettings settings)
{
    public IHost Build()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(builder.Logging, settings.LogDirectory, "play");

        builder.Services.AddSingleton(settings);
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.DefaultRequestTimeout = TimeSpan.FromSeconds(15);
            var locations = options.ConfigureLocations();
            locations.RouteCacheMaxAge = TimeSpan.Zero;
            locations.MessageFollowDuration = TimeSpan.FromSeconds(5);
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = settings.RedisEndpoint;
                redis.KeyPrefix = settings.RedisKeyPrefix;
            }));
            options.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
            {
                redis.ConnectionString = settings.RedisEndpoint;
                redis.KeyPrefix = $"{settings.RedisKeyPrefix}relocation:";
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddStreamNode(SampleNodes.ClientStream)
                .Bind(settings.PlayEndpoint)
                .EnableActorDispatch()
                .AddSession<PlaySession>();
            options.AddClientServerChannel(SampleChannels.Api)
                .Client();

            var mesh = options.AddRouteMesh(SampleNodes.Mesh)
                .Listen(settings.MeshEndpoint);
            mesh.Objects().Server()
                .AddEntrySpot<PlayEntrySpot>()
                .AddActorFactory<PlayActor, PlayActorFactory>(
                    SampleTypes.PlayerActor, factory => factory.PreserveStateWith<PlayActorRelocationAdapter>())
                .AddSpotFactory<TicTacToeGame>(
                    SampleTypes.GameSpot, factory => factory.DisableRelocation());
            mesh.Channel(SampleTopics.PlayerMilestoneChannel).Server();
            foreach (var endpoint in settings.PeerMeshEndpoints)
                mesh.PeerConnections.Connect(endpoint);
        });

        return builder.Build();
    }
}
