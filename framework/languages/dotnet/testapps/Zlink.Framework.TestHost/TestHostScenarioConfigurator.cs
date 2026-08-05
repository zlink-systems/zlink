using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;

internal static class TestHostScenarioConfigurator
{
    public static void Configure(IServiceCollection services, TestHostOptions options)
    {
        switch (options.Mode)
        {
            case "idle":
                return;
            case "registry":
                throw new InvalidOperationException("The core registry runtime has been removed.");
            case "channel-server":
                ConfigureChannelServer(services, options);
                return;
            case "channel-client":
                ConfigureChannelClient(services, options);
                return;
            case "channel-subscriber":
                ConfigureChannelSubscriber(services, options);
                return;
            case "channel-publisher":
                ConfigureChannelPublisher(services, options);
                return;
            case "route-server":
                ConfigureRouteServer(services, options);
                return;
            case "route-client":
                ConfigureRouteClient(services, options);
                return;
            case "spot-node":
                ConfigureSpotNode(services, options);
                return;
            case "stream-raw":
                ConfigureStreamRawNode(services, options);
                return;
            case "stream-client":
                ConfigureStreamClient(services, options);
                return;
            default:
                throw new InvalidOperationException($"Unsupported test host mode '{options.Mode}'.");
        }
    }

    private static void ConfigureChannelServer(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddZLinkFramework(framework =>
        {
            var meshName = options.ChannelName
                           ?? throw new InvalidOperationException(
                               "Channel server mode requires --channel-name.");
            var mesh = framework.AddRouteMesh(meshName)
                .Listen(options.ServerEndpoint
                        ?? throw new InvalidOperationException(
                            "Channel server mode requires --server-endpoint."))
                .SetRoutingId(RoutingId.From("dotnet-channel-server"));
            var channel = mesh.Channel(meshName).Server();
            channel
                .AddRequestHandler<TestHostProfileRequestHandler, TestHostProfileRequest, TestHostProfileReply>();
            channel.AddSendHandler<TestHostProfileSendHandler, TestHostProfileSend>();
        });
    }

    private static void ConfigureChannelClient(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddZLinkFramework(framework =>
        {
            var meshName = options.ChannelName
                           ?? throw new InvalidOperationException(
                               "Channel client mode requires --channel-name.");
            var mesh = framework.AddRouteMesh(meshName)
                .Listen(0)
                .SetRoutingId(RoutingId.From("dotnet-channel-client"));
            mesh.Channel(meshName).Client();
            mesh.PeerConnections.Connect(
                options.ServerEndpoint
                ?? throw new InvalidOperationException(
                    "Channel client mode requires --server-endpoint."));
        });
        services.AddHostedService(provider =>
            new ChannelClientStartupRequestHostedService(
                provider.GetRequiredService<IZLinkRouteClient>(),
                provider.GetRequiredService<TestHostEventSink>(),
                options.ChannelName!,
                options.PublishValue ?? "dotnet-to-node"));
    }

    private static void ConfigureChannelSubscriber(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddZLinkFramework(framework =>
        {
            var channel = framework.AddFanoutChannel(options.ChannelName
                                                     ?? throw new InvalidOperationException(
                                                         "Channel subscriber mode requires --channel-name."));
            channel.Connect(
                options.PublisherEndpoint
                ?? throw new InvalidOperationException(
                    "Channel subscriber mode requires --publisher-endpoint."));
            channel.AddHandler<ChannelSubscriptionEventHandler, TestHostPublishedEvent>();
        });
    }

    private static void ConfigureChannelPublisher(IServiceCollection services, TestHostOptions options)
    {
        services.AddZLinkFramework(framework =>
        {
            framework.AddFanoutChannel(options.ChannelName
                                       ?? throw new InvalidOperationException(
                                           "Channel publisher mode requires --channel-name."))
                .EnablePublisher(options.PublisherEndpoint
                                 ?? throw new InvalidOperationException(
                                     "Channel publisher mode requires --publisher-endpoint."));
        });

        if (!string.IsNullOrWhiteSpace(options.PublishTopic))
            services.AddHostedService(provider =>
                new ChannelStartupPublishHostedService(
                    provider.GetRequiredService<IZLinkFanoutClient>(),
                    options.ChannelName!,
                    options.PublishTopic!,
                    options.PublishValue ?? "startup"));
    }

    private static void ConfigureRouteServer(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddZLinkFramework(framework =>
        {
            var meshName = options.ChannelName
                           ?? throw new InvalidOperationException(
                               "Route server mode requires --channel-name.");
            var mesh = framework.AddRouteMesh(meshName)
                .Listen(options.ServerEndpoint
                        ?? throw new InvalidOperationException(
                            "Route server mode requires --server-endpoint."))
                .SetRoutingId(RoutingId.From("dotnet-route"));
            mesh.Channel(meshName).Client();
            mesh.AddRouteRequestHandler<TestHostRouteRequestHandler, TestHostRouteRequest, TestHostRouteReply>();
        });
    }

    private static void ConfigureRouteClient(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddZLinkFramework(framework =>
        {
            var meshName = options.ChannelName
                           ?? throw new InvalidOperationException(
                               "Route client mode requires --channel-name.");
            var mesh = framework.AddRouteMesh(meshName)
                .Listen(0)
                .SetRoutingId(RoutingId.From("dotnet-route-client"));
            mesh.Channel(meshName).Client();
            mesh.PeerConnections.Connect(
                RoutingId.From("node-route"),
                options.ServerEndpoint
                ?? throw new InvalidOperationException(
                    "Route client mode requires --server-endpoint."));
        });
        services.AddHostedService(provider =>
            new RouteClientStartupRequestHostedService(
                provider.GetRequiredService<IZLinkRouteClient>(),
                provider.GetRequiredService<TestHostEventSink>(),
                options.ChannelName!,
                options.PublishValue ?? "dotnet-route-to-node"));
    }

    private static void ConfigureSpotNode(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddScoped<StartupStageSubscriptionHandler>();
        services.AddZLinkFramework(framework =>
        {
            {
                var mesh = framework.AddRouteMesh(options.DiscoveryChannelName
                                                  ?? throw new InvalidOperationException(
                                                      "SPOT node mode requires --discovery-channel."));
                mesh.Channel(options.DiscoveryChannelName
                             ?? throw new InvalidOperationException(
                                 "SPOT node mode requires --discovery-channel."))
                    .Client();
                _ = options.SpotNodeName
                    ?? throw new InvalidOperationException("SPOT node mode requires --spot-node-name.");
                var spotBindEndpoint = options.SpotBindEndpoint
                                       ?? throw new InvalidOperationException(
                                           "SPOT node mode requires --spot-bind-endpoint.");
                mesh.Listen(spotBindEndpoint);

                if (options.EnableSpotFactory)
                    mesh.Objects().Server().AddSpotFactory<StartupStageSpot>(
                        "startup-stage", factory => factory.DisableRelocation());
            }
        });

        if (options.CreateSpot)
            services.AddHostedService(provider =>
                new StartupSpotCreationHostedService(
                    provider.GetRequiredService<IZLinkSpotManager>(),
                    options.DiscoveryChannelName
                    ?? throw new InvalidOperationException(
                        "SPOT node mode requires --discovery-channel.")));

        if (!string.IsNullOrWhiteSpace(options.AttachSpotPublisherChannel)
            && !string.IsNullOrWhiteSpace(options.PublishTopic))
            services.AddHostedService(provider =>
                new SpotStartupPublishHostedService(
                    provider.GetRequiredService<IZLinkSpotPublisherClient>(),
                    options.AttachSpotPublisherChannel!,
                    options.PublishTopic!,
                    options.PublishValue ?? "startup"));
    }

    private static void ConfigureStreamRawNode(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddSingleton<TestHostRawStreamRecorder>();
        if (!string.IsNullOrWhiteSpace(options.EventFilePath))
        {
            services.AddSingleton(
                new TestHostMessageFlowListener(options.EventFilePath + ".flow"));
        }
        services.AddZLinkFramework(framework =>
        {
            if (!string.IsNullOrWhiteSpace(options.EventFilePath))
                framework.ConfigureDispatch().Diagnostics
                    .SetLevel(ZLinkDiagnosticsLevel.Normal);
            {
                var stream = framework.AddStreamNode("stream.raw");
                stream.Bind(options.StreamEndpoint
                            ?? throw new InvalidOperationException("STREAM raw mode requires --stream-endpoint."));
                stream.AddSession<TestHostRawStreamSession>();
            }
        });
    }

    private static void ConfigureStreamClient(IServiceCollection services, TestHostOptions options)
    {
        services.AddSingleton(new TestHostEventSink(options.EventFilePath));
        services.AddHostedService(provider =>
            new StreamClientStartupRequestHostedService(
                provider.GetRequiredService<TestHostEventSink>(),
                options.StreamEndpoint
                ?? throw new InvalidOperationException("STREAM client mode requires --stream-endpoint."),
                options.PublishValue ?? "dotnet-to-node"));
    }
}
