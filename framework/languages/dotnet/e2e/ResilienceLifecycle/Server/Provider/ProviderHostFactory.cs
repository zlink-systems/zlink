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
                .Listen(Require(options.ChannelEndpoint, "ChannelEndpoint"))
                .SetRoutingIdPrefix(options.Rid);
            mesh.Channel(ResilienceLifecycleNames.Channel).Server()
                .SetWeight(options.Weight)
                .AddRequestHandler<ProfileRequestHandler, ProfileReq, ProfileRes>("ProfileReq")
                .AddSendHandler<ProfileCommandHandler, ProfileMsg>("ProfileMsg");
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
}
