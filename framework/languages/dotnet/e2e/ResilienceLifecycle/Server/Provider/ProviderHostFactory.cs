using Microsoft.Extensions.Configuration;

using ResilienceLifecycle.Server.Provider.Handlers;
using ResilienceLifecycle.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;

using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace ResilienceLifecycle.Server.Provider;

internal static class ProviderHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ServerOptions.Parse(args, "provider");
        Directory.CreateDirectory(options.LogDir);

        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console =>
        {
            console.SingleLine = true;
            console.TimestampFormat = "HH:mm:ss.fff ";
        });
        builder.WebHost.UseUrls(options.HttpUrl);
        var evidence = new EvidenceStore(options.Rid, options.EvidenceFile);
        var fault = new FaultState();
        builder.Services.AddSingleton(evidence);
        builder.Services.AddSingleton(fault);
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid,
            flow =>
            {
                if (flow.Phase != "error") return;
                evidence.Add(
                    "dispatch-error"
                    + $"|surface={flow.Surface}"
                    + $"|kind={flow.MessageKind}"
                    + $"|reason={flow.Reason}"
                    + $"|action={flow.Action}"
                    + $"|packet={flow.PacketName ?? "<null>"}"
                    + $"|channel={flow.ChannelName ?? "<null>"}");
                if (fault.Mode == "observer-throws")
                    throw new InvalidOperationException("dispatch observer failure");
            }));

        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            if (!string.IsNullOrWhiteSpace(options.RedisEndpoint))
                framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix
                                  ?? throw new InvalidOperationException("Shared.RedisKeyPrefix is required."); }));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            var mesh = framework.AddRouteMesh(ResilienceLifecycleNames.Channel)
                .SetRoutingIdPrefix(options.Rid);
            if (string.IsNullOrWhiteSpace(options.ChannelBindHost))
                mesh.Listen(Require(options.ChannelEndpoint, "ChannelEndpoint"));
            else
                mesh.Listen(ParsePort(options.ChannelEndpoint))
                    .SetBindHost(options.ChannelBindHost);
            if (!string.IsNullOrWhiteSpace(options.ChannelAdvertiseHost))
                mesh.SetAdvertiseHost(options.ChannelAdvertiseHost);
            mesh.Channel(ResilienceLifecycleNames.Channel).Server()
                .SetWeight(options.Weight)
                .AddRequestHandler<ProfileRequestHandler, ProfileReq, ProfileRes>("ProfileReq")
                .AddSendHandler<ProfileCommandHandler, ProfileMsg>("ProfileMsg");

            if (options.ClientServerEnabled)
            {
                var server = framework.AddClientServerChannel(
                        ResilienceLifecycleNames.ClientServerChannel)
                    .Server()
                    .Listen(ParsePort(options.ClientServerEndpoint))
                    .SetBindHost(options.ClientServerBindHost ?? "127.0.0.1")
                    .SetAdvertiseHost(options.ClientServerAdvertiseHost ?? "127.0.0.1")
                    .SetWeight(options.Weight);
                server.AddRequestHandler<ProfileRequestHandler, ProfileReq, ProfileRes>("ProfileReq");
            }
        });

        var app = builder.Build();
        app.MapProviderEndpoints(options);
        return app;
    }

    private static string Require(string? value, string name)
    {
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"{name} is required.")
            : value;
    }

    private static int ParsePort(string? endpoint)
    {
        if (!Uri.TryCreate(endpoint, UriKind.Absolute, out var uri)
            || uri.Port is < 1 or > 65535)
            throw new InvalidOperationException("ClientServerEndpoint must contain a valid TCP port.");
        return uri.Port;
    }
}
