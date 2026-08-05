using Microsoft.Extensions.Configuration;
using Systems.Zlink;

using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using SupportChat.Server.Configuration;
using SupportChat.Server.Session.Sessions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Samples.Logging;

namespace SupportChat.Server.Session;

public static class SessionServerHostFactory
{
    public static IHost Build(
        SampleTopology topology,
        SampleSessionNode session,
        string logDirectory)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            logDirectory,
            "session");
        builder.Services.AddSingleton(topology);
        builder.Services.AddZLinkFramework(options =>
        {
            // Channel clients wire through Redis discovery; the session's first
            // authenticate can arrive before the api-channel dealer connects,
            // so the submit window covers the discovery hand-off.
            options.DefaultSocketSendTimeout = TimeSpan.FromSeconds(10);
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(SessionServerHostFactory));
            var mesh = options.AddRouteMesh(SampleNames.MeshName)
                .Listen(session.MeshEndpoint)
                .SetRoutingIdPrefix("support-session");
            mesh.Objects().Client();
            options.AddClientServerChannel(SampleNames.ApiChannel).Client();
            options.AddStreamNode(SampleNames.StreamNode)
                .Bind(session.StreamEndpoint)
                .EnableActorDispatch()
                .AddSession<SupportChatSession>();
        });

        return builder.Build();
    }
}
